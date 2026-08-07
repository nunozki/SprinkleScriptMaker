#pragma once

// Renders a parsed Source Engine studio model (.mdl/.vtx/.vvd) using the legacy
// OpenGL fixed-function pipeline (glBegin/glEnd, glMaterial, glLight). Those
// entry points are exported directly by opengl32.dll on Windows (GL 1.1), so
// no extra function loader is required here - only the FBO calls elsewhere in
// this project need the manual wglGetProcAddress loading done in ViewportFBO.h.

#include <GL/gl.h>
#include <string>
#include <vector>
#include <limits>

#include <mdlpp/mdlpp.h>

#include "SourceAssetManager.h"

struct ModelRenderer {
	mdlpp::StudioModel studioModel;
	mdlpp::BakedModel bakedModel;
	bool loaded = false;
	std::string lastError;

	// Bounding info, used to center the model in the preview viewport.
	float centerX = 0.0f, centerY = 0.0f, centerZ = 0.0f;
	float boundingRadius = 1.0f;

	// Loads and bakes a model by its indexed path (e.g. "models/props/foo.mdl").
	// Returns true on success; on failure, lastError explains why and any
	// previously loaded model is cleared.
	bool Load(const std::string& modelPath) {
		loaded = false;
		bakedModel = mdlpp::BakedModel{};

		// mdlpp::StudioModel::open() refuses to run again once it has already
		// been opened once (its internal `opened` flag latches to true), so a
		// fresh instance is required every time a different model is loaded -
		// otherwise every model after the first one would "fail to parse".
		studioModel = mdlpp::StudioModel{};

		std::vector<std::byte> mdlBytes, vtxBytes, vvdBytes;
		if (!SourceAssetManager::LoadStudioModelBytes(modelPath, mdlBytes, vtxBytes, vvdBytes, lastError)) {
			return false;
		}

		if (!studioModel.open(mdlBytes, vtxBytes, vvdBytes)) {
			lastError = "mdlpp failed to parse the model (corrupt or unsupported version): " + modelPath;
			return false;
		}

		bakedModel = studioModel.processModelData();
		if (bakedModel.vertices.empty() || bakedModel.meshes.empty()) {
			lastError = "Model has no renderable geometry: " + modelPath;
			return false;
		}

		ComputeBounds();
		lastError.clear();
		loaded = true;
		return true;
	}

	void Clear() {
		loaded = false;
		bakedModel = mdlpp::BakedModel{};
		studioModel = mdlpp::StudioModel{};
		lastError.clear();
	}

	// Draws the currently loaded model centered at the origin.
	// Assumes the caller has already set up the projection/modelview matrices.
	void Draw() const {
		if (!loaded) return;

		glPushMatrix();
		glTranslatef(-centerX, -centerY, -centerZ);

		glEnable(GL_LIGHTING);
		glEnable(GL_LIGHT0);
		glEnable(GL_COLOR_MATERIAL);
		glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
		glShadeModel(GL_SMOOTH);

		GLfloat lightDir[4] = { 0.4f, 1.0f, 0.6f, 0.0f }; // directional light
		glLightfv(GL_LIGHT0, GL_POSITION, lightDir);
		GLfloat ambient[4] = { 0.25f, 0.25f, 0.25f, 1.0f };
		glLightfv(GL_LIGHT0, GL_AMBIENT, ambient);

		glColor3f(0.8f, 0.8f, 0.8f); // neutral gray, since materials/textures aren't loaded yet

		for (const auto& mesh : bakedModel.meshes) {
			glBegin(GL_TRIANGLES);
			for (uint16_t index : mesh.indices) {
				const auto& vert = bakedModel.vertices[index];
				glNormal3fv(vert.normal.data());
				glVertex3fv(vert.position.data());
			}
			glEnd();
		}

		glDisable(GL_LIGHTING);
		glDisable(GL_COLOR_MATERIAL);

		glPopMatrix();
	}

private:
	void ComputeBounds() {
		float minX = std::numeric_limits<float>::max(), minY = minX, minZ = minX;
		float maxX = std::numeric_limits<float>::lowest(), maxY = maxX, maxZ = maxX;

		for (const auto& vert : bakedModel.vertices) {
			const auto& p = vert.position;
			minX = std::min(minX, p[0]); maxX = std::max(maxX, p[0]);
			minY = std::min(minY, p[1]); maxY = std::max(maxY, p[1]);
			minZ = std::min(minZ, p[2]); maxZ = std::max(maxZ, p[2]);
		}

		centerX = (minX + maxX) * 0.5f;
		centerY = (minY + maxY) * 0.5f;
		centerZ = (minZ + maxZ) * 0.5f;

		float dx = maxX - minX, dy = maxY - minY, dz = maxZ - minZ;
		boundingRadius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
		if (boundingRadius <= 0.0f) boundingRadius = 1.0f;
	}
};
