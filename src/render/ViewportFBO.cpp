#include "render/ViewportFBO.h"

#include <Windows.h>
#include <GL/gl.h>

// This project has no OpenGL loader (glad/glew). Windows' opengl32.dll only exports
// OpenGL 1.1, so the framebuffer functions are resolved by hand with wglGetProcAddress.

namespace {

	// GL constants missing from the GL 1.1 <GL/gl.h>
	constexpr GLenum kGlFramebuffer = 0x8D40;          // GL_FRAMEBUFFER
	constexpr GLenum kGlRenderbuffer = 0x8D41;         // GL_RENDERBUFFER
	constexpr GLenum kGlColorAttachment0 = 0x8CE0;     // GL_COLOR_ATTACHMENT0
	constexpr GLenum kGlDepthStencilAttachment = 0x821A; // GL_DEPTH_STENCIL_ATTACHMENT
	constexpr GLenum kGlDepth24Stencil8 = 0x88F0;      // GL_DEPTH24_STENCIL8

	using GenFramebuffersProc = void(APIENTRY*)(GLsizei n, GLuint* framebuffers);
	using BindFramebufferProc = void(APIENTRY*)(GLenum target, GLuint framebuffer);
	using FramebufferTexture2DProc = void(APIENTRY*)(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
	using GenRenderbuffersProc = void(APIENTRY*)(GLsizei n, GLuint* renderbuffers);
	using BindRenderbufferProc = void(APIENTRY*)(GLenum target, GLuint renderbuffer);
	using RenderbufferStorageProc = void(APIENTRY*)(GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
	using FramebufferRenderbufferProc = void(APIENTRY*)(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer);
	using DeleteFramebuffersProc = void(APIENTRY*)(GLsizei n, const GLuint* framebuffers);
	using DeleteRenderbuffersProc = void(APIENTRY*)(GLsizei n, const GLuint* renderbuffers);

	struct FboFunctions {
		GenFramebuffersProc genFramebuffers = nullptr;
		BindFramebufferProc bindFramebuffer = nullptr;
		FramebufferTexture2DProc framebufferTexture2D = nullptr;
		GenRenderbuffersProc genRenderbuffers = nullptr;
		BindRenderbufferProc bindRenderbuffer = nullptr;
		RenderbufferStorageProc renderbufferStorage = nullptr;
		FramebufferRenderbufferProc framebufferRenderbuffer = nullptr;
		DeleteFramebuffersProc deleteFramebuffers = nullptr;
		DeleteRenderbuffersProc deleteRenderbuffers = nullptr;
	};

	template <typename Proc>
	Proc LoadProc(const char* name) {
		return reinterpret_cast<Proc>(wglGetProcAddress(name));
	}

	// Resolves the functions on first use (requires a current GL context).
	const FboFunctions& Gl() {
		static const FboFunctions functions = [] {
			FboFunctions f;
			f.genFramebuffers = LoadProc<GenFramebuffersProc>("glGenFramebuffers");
			f.bindFramebuffer = LoadProc<BindFramebufferProc>("glBindFramebuffer");
			f.framebufferTexture2D = LoadProc<FramebufferTexture2DProc>("glFramebufferTexture2D");
			f.genRenderbuffers = LoadProc<GenRenderbuffersProc>("glGenRenderbuffers");
			f.bindRenderbuffer = LoadProc<BindRenderbufferProc>("glBindRenderbuffer");
			f.renderbufferStorage = LoadProc<RenderbufferStorageProc>("glRenderbufferStorage");
			f.framebufferRenderbuffer = LoadProc<FramebufferRenderbufferProc>("glFramebufferRenderbuffer");
			f.deleteFramebuffers = LoadProc<DeleteFramebuffersProc>("glDeleteFramebuffers");
			f.deleteRenderbuffers = LoadProc<DeleteRenderbuffersProc>("glDeleteRenderbuffers");
			return f;
		}();
		return functions;
	}

} // namespace

void ViewportFBO::Init(int w, int h) {
	width = w;
	height = h;

	// Framebuffer
	Gl().genFramebuffers(1, &fbo);
	Gl().bindFramebuffer(kGlFramebuffer, fbo);

	// Color texture
	glGenTextures(1, &textureColorBuffer);
	glBindTexture(GL_TEXTURE_2D, textureColorBuffer);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	Gl().framebufferTexture2D(kGlFramebuffer, kGlColorAttachment0, GL_TEXTURE_2D, textureColorBuffer, 0);

	// Depth/stencil renderbuffer
	Gl().genRenderbuffers(1, &rbo);
	Gl().bindRenderbuffer(kGlRenderbuffer, rbo);
	Gl().renderbufferStorage(kGlRenderbuffer, kGlDepth24Stencil8, width, height);
	Gl().framebufferRenderbuffer(kGlFramebuffer, kGlDepthStencilAttachment, kGlRenderbuffer, rbo);

	Gl().bindFramebuffer(kGlFramebuffer, 0);
}

void ViewportFBO::Resize(int w, int h) {
	if (w <= 0 || h <= 0 || (width == w && height == h)) return;
	width = w;
	height = h;

	glBindTexture(GL_TEXTURE_2D, textureColorBuffer);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);

	Gl().bindRenderbuffer(kGlRenderbuffer, rbo);
	Gl().renderbufferStorage(kGlRenderbuffer, kGlDepth24Stencil8, width, height);
}

void ViewportFBO::Cleanup() {
	if (fbo) Gl().deleteFramebuffers(1, &fbo);
	if (textureColorBuffer) glDeleteTextures(1, &textureColorBuffer);
	if (rbo) Gl().deleteRenderbuffers(1, &rbo);
	fbo = textureColorBuffer = rbo = 0;
}

void ViewportFBO::Bind() const {
	Gl().bindFramebuffer(kGlFramebuffer, fbo);
}

void ViewportFBO::Unbind() {
	Gl().bindFramebuffer(kGlFramebuffer, 0);
}
