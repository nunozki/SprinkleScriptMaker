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
#include <sstream>
#include <cstdlib>
#include <cctype>

#include <mdlpp/mdlpp.h>
#include <vtfpp/VTF.h>
#include <kvpp/KV1.h>
#include <sourcepp/String.h>

#include "SourceAssetManager.h"

// Holds everything about a resolved material that affects how it's drawn:
// not just the base texture, but the handful of $params that commonly show
// up in VMTs and change render state (transparency, additive blending,
// alpha testing, backface culling, and color tinting).
struct MaterialGpuData {
	GLuint textureId = 0;
	bool translucent = false;
	bool additive = false;
	bool alphaTest = false;
	float alphaTestReference = 0.7f; // Source's own default for $alphatestreference
	bool noCull = false;
	float color[3] = { 1.0f, 1.0f, 1.0f };
};


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

		// Material indices are per-model, so cached textures from whatever model
		// was previously loaded are meaningless (and would be wrongly reused) for
		// the new model - drop them so materials get re-resolved and reloaded.
		ReleaseTextures();

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

		ReleaseTextures();
	}

private:
	void ReleaseTextures() {
		for (const auto& [materialIndex, data] : materialData) {
			if (data.textureId != 0) {
				GLuint id = data.textureId;
				glDeleteTextures(1, &id);
			}
		}
		materialData.clear();
	}

public:

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
			const MaterialGpuData& matData = GetOrLoadMaterialData(mesh.materialIndex);
			if (matData.textureId != 0) {
				glEnable(GL_TEXTURE_2D);
				glBindTexture(GL_TEXTURE_2D, matData.textureId);
			}
			else {
				glDisable(GL_TEXTURE_2D);
			}

			// $translucent / $additive -> alpha blending, with additive using a
			// non-darkening blend mode so overlapping geometry brightens instead
			// of occluding, matching how Source renders these shaders.
			if (matData.translucent || matData.additive) {
				glEnable(GL_BLEND);
				glDepthMask(GL_FALSE);
				if (matData.additive) {
					glBlendFunc(GL_SRC_ALPHA, GL_ONE);
				}
				else {
					glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
				}
			}
			else {
				glDisable(GL_BLEND);
				glDepthMask(GL_TRUE);
			}

			// $alphatest -> discard fragments below the reference alpha instead of blending,
			// used for things like foliage/fences that need hard-edged cutouts.
			if (matData.alphaTest) {
				glEnable(GL_ALPHA_TEST);
				glAlphaFunc(GL_GREATER, matData.alphaTestReference);
			}
			else {
				glDisable(GL_ALPHA_TEST);
			}

			// $nocull -> render both faces, used for thin double-sided geometry like leaves.
			if (matData.noCull) {
				glDisable(GL_CULL_FACE);
			}
			else {
				glEnable(GL_CULL_FACE);
				glCullFace(GL_BACK);
			}

			// $color / $color2 tint multiplies the vertex color, which GL_COLOR_MATERIAL
			// feeds into the lighting equation as the ambient/diffuse material color.
			glColor3f(matData.color[0], matData.color[1], matData.color[2]);

			glBegin(GL_TRIANGLES);
			for (uint16_t index : mesh.indices) {
				// Defensive bounds-check: mdlpp's processModelData() can compute indices
				// (via meshVertexID + mesh/model vertex offsets) that assume the full,
				// un-fixed-up VVD vertex layout, while bakedModel.vertices may be a smaller
				// LOD-fixup-filtered subset. Skip any out-of-range index rather than crash.
				if (index >= bakedModel.vertices.size()) {
					continue;
				}
				const auto& vert = bakedModel.vertices[index];
				glNormal3fv(vert.normal.data());
				glTexCoord2f(vert.uv[0], vert.uv[1]);
				glVertex3fv(vert.position.data());
			}
			glEnd();
		}

		glDisable(GL_TEXTURE_2D);
		glDisable(GL_BLEND);
		glDisable(GL_ALPHA_TEST);
		glDisable(GL_CULL_FACE);
		glDepthMask(GL_TRUE);
		glDisable(GL_LIGHTING);
		glDisable(GL_COLOR_MATERIAL);

		glPopMatrix();
	}

private:
	// Resolves (and caches) the render data for a baked mesh's material index, by locating
	// the material's .vmt, loading the referenced $basetexture .vtf, and reading the handful
	// of $params that affect render state. Returns a default (opaque, white, no texture)
	// MaterialGpuData if nothing could be resolved.
	const MaterialGpuData& GetOrLoadMaterialData(int32_t materialIndex) const {
		static const MaterialGpuData kFallback{};
		if (materialIndex < 0 || static_cast<size_t>(materialIndex) >= studioModel.mdl.materials.size()) {
			return kFallback;
		}

		auto cached = materialData.find(materialIndex);
		if (cached != materialData.end()) return cached->second;

		MaterialGpuData data = LoadMaterialDataFromDisk(materialIndex);
		return materialData.emplace(materialIndex, data).first->second;
	}

	MaterialGpuData LoadMaterialDataFromDisk(int32_t materialIndex) const {
		MaterialGpuData result;
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
				// The root KV1 object's only child is the shader block (e.g. "LightmappedGeneric"),
				// so $basetexture and friends live one level deeper, inside that shader block.
				if (kv.getChildCount() == 0) {
					std::cerr << "[ModelRenderer] VMT has no shader block: " << vmtPath << "\n";
					continue;
				}
				const auto& shaderBlock = kv[0u];

				// Patch shaders ("patch" { "include" "materials/foo.vmt" "insert" { ... } })
				// don't carry their own $basetexture - they layer overrides onto another VMT.
				// Follow the include so those materials still render instead of being skipped.
				if (sourcepp::string::iequals(shaderBlock.getKey(), "patch") && shaderBlock.hasChild("include")) {
					std::string includePath = std::string(shaderBlock["include"].getValue());
					for (auto& c : includePath) if (c == '\\') c = '/';

					std::vector<std::byte> includeBytes;
					std::string includeErr;
					if (SourceAssetManager::LoadAssetBytes(includePath, includeBytes, includeErr)) {
						std::string includeText(reinterpret_cast<const char*>(includeBytes.data()), includeBytes.size());
						kvpp::KV1 includeKv{includeText};
						if (includeKv.getChildCount() > 0) {
							result = ParseShaderBlock(includeKv[0u], materialName, vmtPath);
							if (result.textureId != 0) return result;
						}
					}
					else {
						std::cerr << "[ModelRenderer] Patch VMT include not found: " << includePath << " (" << includeErr << ")\n";
					}
					continue;
				}

				result = ParseShaderBlock(shaderBlock, materialName, vmtPath);
				if (result.textureId != 0) return result;
			}
			catch (const std::exception& e) {
				std::cerr << "[ModelRenderer] Exception parsing VMT " << vmtPath << ": " << e.what() << "\n";
				continue;
			}
		}

		if (!foundAnyVmt) {
			std::cerr << "[ModelRenderer] Could not find any .vmt for material \"" << materialName << "\" (tried " << dirsToTry.size() << " material director" << (dirsToTry.size() == 1 ? "y" : "ies") << ")\n";
		}

		return result;
	}

	// Reads $basetexture plus the handful of $params that affect render state out of a
	// single shader block (e.g. the body of "LightmappedGeneric" { ... }), and uploads the
	// referenced .vtf as a GL texture. Returns a MaterialGpuData with textureId == 0 if the
	// texture couldn't be resolved (caller tries the next material directory / VMT).
	MaterialGpuData ParseShaderBlock(const kvpp::KV1ElementReadable<>& shaderBlock, const std::string& materialName, const std::string& vmtPath) const {
		MaterialGpuData data;

		std::string baseTexture;
		for (const char* key : {"$basetexture", "$baseTexture", "$BaseTexture"}) {
			if (shaderBlock.hasChild(key)) {
				baseTexture = std::string(shaderBlock[key].getValue());
				break;
			}
		}
		if (baseTexture.empty()) {
			std::cerr << "[ModelRenderer] VMT has no $basetexture: " << vmtPath << "\n";
			return data;
		}

		for (auto& c : baseTexture) if (c == '\\') c = '/';
		std::string vtfPath = "materials/" + baseTexture + ".vtf";

		std::vector<std::byte> vtfBytes;
		std::string err;
		if (!SourceAssetManager::LoadAssetBytes(vtfPath, vtfBytes, err)) {
			std::cerr << "[ModelRenderer] VTF not found: " << vtfPath << " (" << err << ")\n";
			return data;
		}

		vtfpp::VTF vtf{std::move(vtfBytes)};
		if (!vtf) {
			std::cerr << "[ModelRenderer] vtfpp failed to parse VTF: " << vtfPath << "\n";
			return data;
		}

		uint16_t width = vtf.getWidth();
		uint16_t height = vtf.getHeight();
		std::vector<std::byte> rgba = vtf.getImageDataAsRGBA8888();
		if (rgba.empty() || width == 0 || height == 0) {
			std::cerr << "[ModelRenderer] VTF decoded but produced no pixel data: " << vtfPath << "\n";
			return data;
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
		data.textureId = texId;

		// --- Extra $params that affect render state ---
		data.translucent = GetBoolParam(shaderBlock, "$translucent", false);
		data.additive = GetBoolParam(shaderBlock, "$additive", false);
		data.alphaTest = GetBoolParam(shaderBlock, "$alphatest", false);
		data.alphaTestReference = GetFloatParam(shaderBlock, "$alphatestreference", data.alphaTestReference);
		// $nocull, and the common typo/alias $noculling, both show up in the wild.
		data.noCull = GetBoolParam(shaderBlock, "$nocull", false) || GetBoolParam(shaderBlock, "$noculling", false);

		for (const char* key : {"$color2", "$color"}) {
			if (shaderBlock.hasChild(key)) {
				float rgb[3];
				if (ParseColorParam(std::string(shaderBlock[key].getValue()), rgb)) {
					data.color[0] = rgb[0];
					data.color[1] = rgb[1];
					data.color[2] = rgb[2];
					break;
				}
			}
		}

		std::cerr << "[ModelRenderer] Loaded texture " << vtfPath << " (" << width << "x" << height << ") for material \"" << materialName << "\""
			<< (data.translucent ? " [translucent]" : "")
			<< (data.additive ? " [additive]" : "")
			<< (data.alphaTest ? " [alphatest]" : "")
			<< (data.noCull ? " [nocull]" : "")
			<< "\n";
		return data;
	}

	static bool GetBoolParam(const kvpp::KV1ElementReadable<>& block, const char* key, bool defaultValue) {
		if (!block.hasChild(key)) return defaultValue;
		std::string value = std::string(block[key].getValue());
		// VMT booleans are conventionally "0"/"1", but be lenient about whitespace/case.
		for (auto& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return value == "1" || value == "true";
	}

	static float GetFloatParam(const kvpp::KV1ElementReadable<>& block, const char* key, float defaultValue) {
		if (!block.hasChild(key)) return defaultValue;
		try {
			return std::stof(std::string(block[key].getValue()));
		}
		catch (...) {
			return defaultValue;
		}
	}

	// Parses VMT color values, which appear either as "[r g b]" (0-1 floats) or
	// "{r g b}" (0-255 ints), or occasionally a single scalar for grayscale tinting.
	static bool ParseColorParam(std::string value, float outRgb[3]) {
		bool isByteRange = false;
		for (char& c : value) {
			if (c == '[') { c = ' '; }
			else if (c == '{') { c = ' '; isByteRange = true; }
			else if (c == ']' || c == '}' || c == ',') { c = ' '; }
		}

		std::istringstream stream(value);
		std::vector<float> values;
		float v;
		while (stream >> v) values.push_back(v);

		if (values.empty()) return false;

		if (values.size() == 1) {
			float scalar = isByteRange ? values[0] / 255.0f : values[0];
			outRgb[0] = outRgb[1] = outRgb[2] = scalar;
			return true;
		}
		if (values.size() >= 3) {
			for (int i = 0; i < 3; ++i) {
				outRgb[i] = isByteRange ? values[i] / 255.0f : values[i];
			}
			return true;
		}
		return false;
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

	mutable std::unordered_map<int32_t, MaterialGpuData> materialData;
};
