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
#include <unordered_map>
#include <iostream>

#include <mdlpp/mdlpp.h>
#include <vtfpp/VTF.h>
#include <kvpp/KV1.h>

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

		for (const auto& [materialIndex, texId] : materialTextures) {
			if (texId != 0) {
				GLuint id = texId;
				glDeleteTextures(1, &id);
			}
		}
		materialTextures.clear();
	}

	// Draws the currently loaded model centered at the origin.
	// Assumes the caller has already set up the projection/modelview matrices.
	void Draw() const {
		if (!loaded) return;

		glPushMatrix();

		// Source Engine models are authored Z-up; OpenGL's default camera/view setup in this
		// project expects Y-up, so without this rotation every model renders "lying on its
		// side". Rotating -90 degrees about X turns Z-up into Y-up (Z becomes -Y, Y becomes Z).
		glRotatef(-90.0f, 1.0f, 0.0f, 0.0f);
		// Deliberately NOT translating by -center here: the model is drawn in its own local
		// space so its origin (0,0,0) lines up with the viewport/grid origin, matching where
		// the model would sit in-game rather than being re-centered on its bounding box.

		glEnable(GL_LIGHTING);
		glEnable(GL_LIGHT0);
		glEnable(GL_COLOR_MATERIAL);
		glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
		glShadeModel(GL_SMOOTH);

		GLfloat lightDir[4] = { 0.4f, 1.0f, 0.6f, 0.0f }; // directional light
		glLightfv(GL_LIGHT0, GL_POSITION, lightDir);
		GLfloat ambient[4] = { 0.25f, 0.25f, 0.25f, 1.0f };
		glLightfv(GL_LIGHT0, GL_AMBIENT, ambient);

		glColor3f(1.0f, 1.0f, 1.0f);

		for (const auto& mesh : bakedModel.meshes) {
			GLuint texId = GetOrLoadMaterialTexture(mesh.materialIndex);
			if (texId != 0) {
				glEnable(GL_TEXTURE_2D);
				glBindTexture(GL_TEXTURE_2D, texId);
			}
			else {
				glDisable(GL_TEXTURE_2D);
			}

			glBegin(GL_TRIANGLES);
			for (uint16_t index : mesh.indices) {
				const auto& vert = bakedModel.vertices[index];
				glNormal3fv(vert.normal.data());
				glTexCoord2f(vert.uv[0], vert.uv[1]);
				glVertex3fv(vert.position.data());
			}
			glEnd();
		}

		glDisable(GL_TEXTURE_2D);
		glDisable(GL_LIGHTING);
		glDisable(GL_COLOR_MATERIAL);

		glPopMatrix();
	}

private:
	// Resolves (and caches) the GL texture for a baked mesh's material index, by locating
	// the material's .vmt (to find $basetexture), then loading that .vtf and uploading its
	// RGBA8888 pixel data as a GL_TEXTURE_2D. Returns 0 if no texture could be resolved.
	GLuint GetOrLoadMaterialTexture(int32_t materialIndex) const {
		if (materialIndex < 0 || static_cast<size_t>(materialIndex) >= studioModel.mdl.materials.size()) {
			return 0;
		}

		auto cached = materialTextures.find(materialIndex);
		if (cached != materialTextures.end()) return cached->second;

		GLuint texId = LoadMaterialTextureFromDisk(materialIndex);
		materialTextures[materialIndex] = texId;
		return texId;
	}

	GLuint LoadMaterialTextureFromDisk(int32_t materialIndex) const {
		const std::string& materialName = studioModel.mdl.materials[materialIndex].name;
		const auto& materialDirs = studioModel.mdl.materialDirectories;

		std::vector<std::string> dirsToTry = materialDirs;
		if (dirsToTry.empty()) dirsToTry.push_back("");

		bool foundAnyVmt = false;
		for (auto dir : dirsToTry) {
			if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';
			for (auto& c : dir) if (c == '\\') c = '/';

			std::string vmtPath = "materials/" + dir + materialName + ".vmt";

			std::vector<std::byte> vmtBytes;
			std::string err;
			if (!SourceAssetManager::LoadAssetBytes(vmtPath, vmtBytes, err)) {
				std::cerr << "[ModelRenderer] VMT not found: " << vmtPath << " (" << err << ")\n";
				continue;
			}
			foundAnyVmt = true;

			std::string vmtText(reinterpret_cast<const char*>(vmtBytes.data()), vmtBytes.size());
			try {
				kvpp::KV1 kv{vmtText};
				std::string baseTexture;
				for (const char* key : {"$basetexture", "$baseTexture", "$BaseTexture"}) {
					if (kv.hasChild(key)) {
						baseTexture = std::string(kv[key].getValue());
						break;
					}
				}
				if (baseTexture.empty()) {
					std::cerr << "[ModelRenderer] VMT has no $basetexture: " << vmtPath << "\n";
					continue;
				}

				for (auto& c : baseTexture) if (c == '\\') c = '/';
				std::string vtfPath = "materials/" + baseTexture + ".vtf";

				std::vector<std::byte> vtfBytes;
				if (!SourceAssetManager::LoadAssetBytes(vtfPath, vtfBytes, err)) {
					std::cerr << "[ModelRenderer] VTF not found: " << vtfPath << " (" << err << ")\n";
					continue;
				}

				vtfpp::VTF vtf{std::move(vtfBytes)};
				if (!vtf) {
					std::cerr << "[ModelRenderer] vtfpp failed to parse VTF: " << vtfPath << "\n";
					continue;
				}

				uint16_t width = vtf.getWidth();
				uint16_t height = vtf.getHeight();
				std::vector<std::byte> rgba = vtf.getImageDataAsRGBA8888();
				if (rgba.empty() || width == 0 || height == 0) {
					std::cerr << "[ModelRenderer] VTF decoded but produced no pixel data: " << vtfPath << "\n";
					continue;
				}

				GLuint texId;
				glGenTextures(1, &texId);
				glBindTexture(GL_TEXTURE_2D, texId);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
				glBindTexture(GL_TEXTURE_2D, 0);
				std::cerr << "[ModelRenderer] Loaded texture " << vtfPath << " (" << width << "x" << height << ") for material \"" << materialName << "\"\n";
				return texId;
			}
			catch (const std::exception& e) {
				std::cerr << "[ModelRenderer] Exception parsing VMT " << vmtPath << ": " << e.what() << "\n";
				continue;
			}
		}

		if (!foundAnyVmt) {
			std::cerr << "[ModelRenderer] Could not find any .vmt for material \"" << materialName << "\" (tried " << dirsToTry.size() << " material director" << (dirsToTry.size() == 1 ? "y" : "ies") << ")\n";
		}

		return 0;
	}
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

	mutable std::unordered_map<int32_t, GLuint> materialTextures;
};
