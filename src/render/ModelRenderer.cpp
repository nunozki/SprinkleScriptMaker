#include "render/ModelRenderer.h"

#include <Windows.h>
#include <GL/gl.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iostream>
#include <limits>
#include <sstream>
#include <vector>

#include <kvpp/KV1.h>
#include <sourcepp/String.h>
#include <vtfpp/VTF.h>

#include "assets/SourceAssetManager.h"

namespace {

	// mdlpp::StudioModel::open() parses data straight from game files. The out-of-range
	// VTX span crash (a fatal, uncatchable 0xC0000409 fail-fast) is fixed at the source by
	// patches/sourcepp-vtx-bounds-check.patch, applied by CMakeLists.txt.
	//
	// This SEH wrapper is defense in depth for *other*, catchable native faults (e.g. access
	// violations) on a corrupt asset, so one bad model fails gracefully instead of taking
	// down the app. It can't catch 0xC0000409. It must stay in its own function: objects
	// with non-trivial destructors aren't allowed in a function that uses __try/__except.
	bool SafeOpenStudioModel(mdlpp::StudioModel& model,
	                         const std::vector<std::byte>& mdlBytes,
	                         const std::vector<std::byte>& vtxBytes,
	                         const std::vector<std::byte>& vvdBytes,
	                         bool& outFaultTrapped) {
		outFaultTrapped = false;
		__try {
			return model.open(mdlBytes, vtxBytes, vvdBytes);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			outFaultTrapped = true;
			return false;
		}
	}

	void NormalizeSlashes(std::string& path) {
		std::replace(path.begin(), path.end(), '\\', '/');
	}

	bool GetBoolParam(const kvpp::KV1ElementReadable<>& block, const char* key, bool defaultValue) {
		if (!block.hasChild(key)) return defaultValue;
		std::string value = std::string(block[key].getValue());
		// VMT booleans are conventionally "0"/"1", but be lenient about case.
		for (auto& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return value == "1" || value == "true";
	}

	float GetFloatParam(const kvpp::KV1ElementReadable<>& block, const char* key, float defaultValue) {
		if (!block.hasChild(key)) return defaultValue;
		try {
			return std::stof(std::string(block[key].getValue()));
		}
		catch (...) {
			return defaultValue;
		}
	}

	// Parses VMT colors, written as "[r g b]" (0-1 floats), "{r g b}" (0-255 ints), or
	// occasionally a single scalar for a grayscale tint.
	bool ParseColorParam(std::string value, float outRgb[3]) {
		bool isByteRange = false;
		for (char& c : value) {
			if (c == '{') { c = ' '; isByteRange = true; }
			else if (c == '[' || c == ']' || c == '}' || c == ',') { c = ' '; }
		}

		std::istringstream stream(value);
		std::vector<float> values;
		float v;
		while (stream >> v) values.push_back(v);

		const float scale = isByteRange ? 1.0f / 255.0f : 1.0f;

		if (values.size() == 1) {
			outRgb[0] = outRgb[1] = outRgb[2] = values[0] * scale;
			return true;
		}
		if (values.size() >= 3) {
			for (int i = 0; i < 3; ++i) {
				outRgb[i] = values[i] * scale;
			}
			return true;
		}
		return false;
	}

	GLuint UploadTexture(uint16_t width, uint16_t height, const std::vector<std::byte>& rgba) {
		GLuint texId = 0;
		glGenTextures(1, &texId);
		glBindTexture(GL_TEXTURE_2D, texId);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
		glBindTexture(GL_TEXTURE_2D, 0);
		return texId;
	}

	// Reads $basetexture plus the render-state $params out of one shader block (e.g. the body
	// of "VertexLitGeneric" { ... }) and uploads the .vtf as a GL texture. Returns textureId == 0
	// if the texture couldn't be resolved, so the caller can try the next candidate.
	MaterialGpuData ParseShaderBlock(const kvpp::KV1ElementReadable<>& shaderBlock, const std::string& materialName, const std::string& vmtPath) {
		MaterialGpuData data;

		std::string baseTexture;
		for (const char* key : { "$basetexture", "$baseTexture", "$BaseTexture" }) {
			if (shaderBlock.hasChild(key)) {
				baseTexture = std::string(shaderBlock[key].getValue());
				break;
			}
		}
		if (baseTexture.empty()) {
			std::cerr << "[ModelRenderer] VMT has no $basetexture: " << vmtPath << "\n";
			return data;
		}

		NormalizeSlashes(baseTexture);
		std::string vtfPath = "materials/" + baseTexture + ".vtf";

		std::vector<std::byte> vtfBytes;
		std::string err;
		if (!SourceAssetManager::LoadAssetBytes(vtfPath, vtfBytes, err)) {
			std::cerr << "[ModelRenderer] VTF not found: " << vtfPath << " (" << err << ")\n";
			return data;
		}

		vtfpp::VTF vtf{ std::move(vtfBytes) };
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

		data.textureId = UploadTexture(width, height, rgba);

		data.translucent = GetBoolParam(shaderBlock, "$translucent", false);
		data.additive = GetBoolParam(shaderBlock, "$additive", false);
		data.alphaTest = GetBoolParam(shaderBlock, "$alphatest", false);
		data.alphaTestReference = GetFloatParam(shaderBlock, "$alphatestreference", data.alphaTestReference);
		// $nocull and the common alias $noculling both show up in the wild.
		data.noCull = GetBoolParam(shaderBlock, "$nocull", false) || GetBoolParam(shaderBlock, "$noculling", false);

		for (const char* key : { "$color2", "$color" }) {
			if (shaderBlock.hasChild(key) && ParseColorParam(std::string(shaderBlock[key].getValue()), data.color)) {
				break;
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

	// Applies a material's texture and render state before drawing a mesh.
	void ApplyMaterialState(const MaterialGpuData& mat) {
		if (mat.textureId != 0) {
			glEnable(GL_TEXTURE_2D);
			glBindTexture(GL_TEXTURE_2D, mat.textureId);
		}
		else {
			glDisable(GL_TEXTURE_2D);
		}

		// $translucent / $additive -> alpha blending. Additive uses a non-darkening blend so
		// overlapping geometry brightens instead of occluding, as in Source.
		if (mat.translucent || mat.additive) {
			glEnable(GL_BLEND);
			glDepthMask(GL_FALSE);
			glBlendFunc(GL_SRC_ALPHA, mat.additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
		}
		else {
			glDisable(GL_BLEND);
			glDepthMask(GL_TRUE);
		}

		// $alphatest -> hard-edged cutouts (foliage, fences) instead of blending.
		if (mat.alphaTest) {
			glEnable(GL_ALPHA_TEST);
			glAlphaFunc(GL_GREATER, mat.alphaTestReference);
		}
		else {
			glDisable(GL_ALPHA_TEST);
		}

		// $nocull -> draw both faces (thin double-sided geometry like leaves).
		if (mat.noCull) {
			glDisable(GL_CULL_FACE);
		}
		else {
			glEnable(GL_CULL_FACE);
			glCullFace(GL_BACK);
		}

		// $color / $color2 tint, fed into lighting through GL_COLOR_MATERIAL.
		glColor3f(mat.color[0], mat.color[1], mat.color[2]);
	}

} // namespace

bool ModelRenderer::Load(const std::string& modelPath) {
	loaded = false;
	bakedModel = mdlpp::BakedModel{};
	currentSkin = 0;

	// StudioModel::open() refuses to run twice on the same instance, so every model needs
	// a fresh one.
	studioModel = mdlpp::StudioModel{};

	// Material indices are per model, so the previous model's textures can't be reused.
	ReleaseTextures();

	std::vector<std::byte> mdlBytes, vtxBytes, vvdBytes;
	if (!SourceAssetManager::LoadStudioModelBytes(modelPath, mdlBytes, vtxBytes, vvdBytes, lastError)) {
		return false;
	}

	bool sehFaultTrapped = false;
	if (!SafeOpenStudioModel(studioModel, mdlBytes, vtxBytes, vvdBytes, sehFaultTrapped)) {
		if (sehFaultTrapped) {
			std::cerr << "[ModelRenderer] Native fault trapped while parsing model: " << modelPath << std::endl;
		}
		lastError = "mdlpp failed to parse the model (corrupt, mismatched, or unsupported MDL/VTX/VVD pair): " + modelPath;
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

void ModelRenderer::Clear() {
	loaded = false;
	bakedModel = mdlpp::BakedModel{};
	studioModel = mdlpp::StudioModel{};
	lastError.clear();
	currentSkin = 0;

	ReleaseTextures();
}

int ModelRenderer::GetSkinCount() const {
	return static_cast<int>(studioModel.mdl.skins.size());
}

void ModelRenderer::SetSkin(int skin) {
	int count = GetSkinCount();
	currentSkin = count <= 0 ? 0 : std::clamp(skin, 0, count - 1);
}

void ModelRenderer::Draw() const {
	if (!loaded) return;

	glPushMatrix();

	// Source models are Z-up; this viewport is Y-up. Rotating -90 degrees about X turns
	// Z-up into Y-up. The model is deliberately not re-centered on its bounding box, so its
	// origin sits on the grid origin, as it would in-game.
	glRotatef(-90.0f, 1.0f, 0.0f, 0.0f);

	glEnable(GL_LIGHTING);
	glEnable(GL_LIGHT0);
	glEnable(GL_COLOR_MATERIAL);
	glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
	glShadeModel(GL_SMOOTH);

	GLfloat lightDir[4] = { 0.4f, 1.0f, 0.6f, 0.0f }; // Directional light
	glLightfv(GL_LIGHT0, GL_POSITION, lightDir);
	GLfloat ambient[4] = { 0.25f, 0.25f, 0.25f, 1.0f };
	glLightfv(GL_LIGHT0, GL_AMBIENT, ambient);

	glColor3f(1.0f, 1.0f, 1.0f);

	for (const auto& mesh : bakedModel.meshes) {
		// Skin families remap a mesh's default material index to another one. The table
		// (mdl.skins[currentSkin]) is indexed by the default material index, like the
		// engine's own pSkinref() lookup.
		int32_t materialIndex = mesh.materialIndex;
		if (currentSkin > 0 && static_cast<std::size_t>(currentSkin) < studioModel.mdl.skins.size()) {
			const auto& skinTable = studioModel.mdl.skins[currentSkin];
			if (materialIndex >= 0 && static_cast<std::size_t>(materialIndex) < skinTable.size()) {
				materialIndex = skinTable[materialIndex];
			}
		}

		ApplyMaterialState(GetOrLoadMaterialData(materialIndex));

		glBegin(GL_TRIANGLES);
		for (uint16_t index : mesh.indices) {
			// processModelData() can produce indices that assume the full VVD vertex layout
			// while bakedModel.vertices is a smaller LOD-fixup subset. Skip those rather than crash.
			if (index >= bakedModel.vertices.size()) continue;

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

void ModelRenderer::ReleaseTextures() {
	for (const auto& [materialIndex, data] : materialData) {
		if (data.textureId != 0) {
			GLuint id = data.textureId;
			glDeleteTextures(1, &id);
		}
	}
	materialData.clear();
}

void ModelRenderer::ComputeBounds() {
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

const MaterialGpuData& ModelRenderer::GetOrLoadMaterialData(int32_t materialIndex) const {
	static const MaterialGpuData kFallback{};
	if (materialIndex < 0 || static_cast<std::size_t>(materialIndex) >= studioModel.mdl.materials.size()) {
		return kFallback;
	}

	auto cached = materialData.find(materialIndex);
	if (cached != materialData.end()) return cached->second;

	return materialData.emplace(materialIndex, LoadMaterialDataFromDisk(materialIndex)).first->second;
}

// Finds the material's .vmt in each of the model's material directories, and resolves its
// texture and render state. Returns an untextured default if nothing could be resolved.
MaterialGpuData ModelRenderer::LoadMaterialDataFromDisk(int32_t materialIndex) const {
	MaterialGpuData result;
	const std::string& materialName = studioModel.mdl.materials[materialIndex].name;

	std::vector<std::string> dirsToTry = studioModel.mdl.materialDirectories;
	if (dirsToTry.empty()) dirsToTry.push_back("");

	bool foundAnyVmt = false;
	for (auto dir : dirsToTry) {
		NormalizeSlashes(dir);
		if (!dir.empty() && dir.back() != '/') dir += '/';

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
			kvpp::KV1 kv{ vmtText };
			// The root's only child is the shader block (e.g. "VertexLitGeneric"), which
			// holds $basetexture and friends.
			if (kv.getChildCount() == 0) {
				std::cerr << "[ModelRenderer] VMT has no shader block: " << vmtPath << "\n";
				continue;
			}
			const auto& shaderBlock = kv[0u];

			// "patch" materials don't have their own $basetexture: they layer overrides onto
			// another VMT. Follow the include so they still render.
			if (sourcepp::string::iequals(shaderBlock.getKey(), "patch") && shaderBlock.hasChild("include")) {
				std::string includePath = std::string(shaderBlock["include"].getValue());
				NormalizeSlashes(includePath);

				std::vector<std::byte> includeBytes;
				std::string includeErr;
				if (SourceAssetManager::LoadAssetBytes(includePath, includeBytes, includeErr)) {
					std::string includeText(reinterpret_cast<const char*>(includeBytes.data()), includeBytes.size());
					kvpp::KV1 includeKv{ includeText };
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
		}
	}

	if (!foundAnyVmt) {
		std::cerr << "[ModelRenderer] Could not find any .vmt for material \"" << materialName << "\" (tried "
		          << dirsToTry.size() << " material director" << (dirsToTry.size() == 1 ? "y" : "ies") << ")\n";
	}

	return result;
}
