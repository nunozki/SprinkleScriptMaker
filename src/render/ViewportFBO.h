#pragma once

// An offscreen framebuffer (color texture + depth/stencil renderbuffer) that the 3D preview
// renders into; the color texture is then shown in the UI with ImGui::Image().
//
// Requires a current OpenGL context for every call.
class ViewportFBO {
public:
	// Creates the framebuffer. Call once, after the GL context is current.
	void Init(int w, int h);

	// Resizes the attachments. Does nothing if the size is unchanged or invalid.
	void Resize(int w, int h);

	// Deletes the GL objects.
	void Cleanup();

	// Makes subsequent draw calls render into this framebuffer.
	void Bind() const;

	// Returns to the default window framebuffer.
	static void Unbind();

	unsigned int GetColorTexture() const { return textureColorBuffer; }
	int GetWidth() const { return width; }
	int GetHeight() const { return height; }

private:
	unsigned int fbo = 0;
	unsigned int textureColorBuffer = 0;
	unsigned int rbo = 0;
	int width = 800;
	int height = 600;
};
