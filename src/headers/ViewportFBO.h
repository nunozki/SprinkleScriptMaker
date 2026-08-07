#pragma once

// NOTE: This project has no OpenGL loader (glad/glew) set up in CMake, and
// <glad/glad.h> is not available. Windows' opengl32.dll only exports OpenGL
// 1.1 functions directly - anything newer (framebuffers, renderbuffers, etc.)
// must be resolved manually via wglGetProcAddress. This header does exactly
// that, just for the handful of functions/constants this file needs, so no
// extra dependency is required.

#include <Windows.h>
#include <GL/gl.h>

// ---- Minimal GL constants not present in the old <GL/gl.h> (GL 1.1) ----
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER 0x8D41
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_DEPTH_STENCIL_ATTACHMENT
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#endif
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8 0x88F0
#endif

// ---- Function pointer typedefs matching the real GL signatures ----
typedef void (APIENTRY* PFNGLGENFRAMEBUFFERSPROC)(GLsizei n, GLuint* framebuffers);
typedef void (APIENTRY* PFNGLBINDFRAMEBUFFERPROC)(GLenum target, GLuint framebuffer);
typedef void (APIENTRY* PFNGLFRAMEBUFFERTEXTURE2DPROC)(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
typedef void (APIENTRY* PFNGLGENRENDERBUFFERSPROC)(GLsizei n, GLuint* renderbuffers);
typedef void (APIENTRY* PFNGLBINDRENDERBUFFERPROC)(GLenum target, GLuint renderbuffer);
typedef void (APIENTRY* PFNGLRENDERBUFFERSTORAGEPROC)(GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
typedef void (APIENTRY* PFNGLFRAMEBUFFERRENDERBUFFERPROC)(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer);
typedef void (APIENTRY* PFNGLDELETEFRAMEBUFFERSPROC)(GLsizei n, const GLuint* framebuffers);
typedef void (APIENTRY* PFNGLDELETERENDERBUFFERSPROC)(GLsizei n, const GLuint* renderbuffers);

namespace detail {
    inline PFNGLGENFRAMEBUFFERSPROC glGenFramebuffers_ = nullptr;
    inline PFNGLBINDFRAMEBUFFERPROC glBindFramebuffer_ = nullptr;
    inline PFNGLFRAMEBUFFERTEXTURE2DPROC glFramebufferTexture2D_ = nullptr;
    inline PFNGLGENRENDERBUFFERSPROC glGenRenderbuffers_ = nullptr;
    inline PFNGLBINDRENDERBUFFERPROC glBindRenderbuffer_ = nullptr;
    inline PFNGLRENDERBUFFERSTORAGEPROC glRenderbufferStorage_ = nullptr;
    inline PFNGLFRAMEBUFFERRENDERBUFFERPROC glFramebufferRenderbuffer_ = nullptr;
    inline PFNGLDELETEFRAMEBUFFERSPROC glDeleteFramebuffers_ = nullptr;
    inline PFNGLDELETERENDERBUFFERSPROC glDeleteRenderbuffers_ = nullptr;
    inline bool fboFunctionsLoaded = false;

    inline void LoadFboFunctions() {
        if (fboFunctionsLoaded) return;
        glGenFramebuffers_ = (PFNGLGENFRAMEBUFFERSPROC)wglGetProcAddress("glGenFramebuffers");
        glBindFramebuffer_ = (PFNGLBINDFRAMEBUFFERPROC)wglGetProcAddress("glBindFramebuffer");
        glFramebufferTexture2D_ = (PFNGLFRAMEBUFFERTEXTURE2DPROC)wglGetProcAddress("glFramebufferTexture2D");
        glGenRenderbuffers_ = (PFNGLGENRENDERBUFFERSPROC)wglGetProcAddress("glGenRenderbuffers");
        glBindRenderbuffer_ = (PFNGLBINDRENDERBUFFERPROC)wglGetProcAddress("glBindRenderbuffer");
        glRenderbufferStorage_ = (PFNGLRENDERBUFFERSTORAGEPROC)wglGetProcAddress("glRenderbufferStorage");
        glFramebufferRenderbuffer_ = (PFNGLFRAMEBUFFERRENDERBUFFERPROC)wglGetProcAddress("glFramebufferRenderbuffer");
        glDeleteFramebuffers_ = (PFNGLDELETEFRAMEBUFFERSPROC)wglGetProcAddress("glDeleteFramebuffers");
        glDeleteRenderbuffers_ = (PFNGLDELETERENDERBUFFERSPROC)wglGetProcAddress("glDeleteRenderbuffers");
        fboFunctionsLoaded = true;
    }
}

struct ViewportFBO {
    unsigned int fbo = 0;
    unsigned int textureColorBuffer = 0;
    unsigned int rbo = 0;
    int width = 800;
    int height = 600;

    void Init(int w, int h) {
        detail::LoadFboFunctions(); // Requires an active GL context (call after glfwMakeContextCurrent)

        width = w;
        height = h;

        // Create Framebuffer
        detail::glGenFramebuffers_(1, &fbo);
        detail::glBindFramebuffer_(GL_FRAMEBUFFER, fbo);

        // Create texture for visual output
        glGenTextures(1, &textureColorBuffer);
        glBindTexture(GL_TEXTURE_2D, textureColorBuffer);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        detail::glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textureColorBuffer, 0);

        // Create Renderbuffer for Depth/Stencil attachments
        detail::glGenRenderbuffers_(1, &rbo);
        detail::glBindRenderbuffer_(GL_RENDERBUFFER, rbo);
        detail::glRenderbufferStorage_(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
        detail::glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbo);

        detail::glBindFramebuffer_(GL_FRAMEBUFFER, 0);
    }

    void Resize(int w, int h) {
        if (w <= 0 || h <= 0 || (width == w && height == h)) return;
        width = w;
        height = h;

        glBindTexture(GL_TEXTURE_2D, textureColorBuffer);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);

        detail::glBindRenderbuffer_(GL_RENDERBUFFER, rbo);
        detail::glRenderbufferStorage_(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    }

    void Cleanup() {
        if (fbo) detail::glDeleteFramebuffers_(1, &fbo);
        if (textureColorBuffer) glDeleteTextures(1, &textureColorBuffer);
        if (rbo) detail::glDeleteRenderbuffers_(1, &rbo);
    }

    // Bind this FBO so subsequent GL draw calls render into it.
    void Bind() const {
        detail::glBindFramebuffer_(GL_FRAMEBUFFER, fbo);
    }

    // Unbind, returning to the default window framebuffer.
    static void Unbind() {
        detail::glBindFramebuffer_(GL_FRAMEBUFFER, 0);
    }
};