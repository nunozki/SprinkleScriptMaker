#pragma once

// Renders a Source Engine studio model (.mdl/.vtx/.vvd) with its VMT/VTF materials, using
// the legacy OpenGL fixed-function pipeline (glBegin/glEnd, glLight). Those entry points
// are exported directly by opengl32.dll on Windows, so no function loader is needed here.
//
// Requires a current OpenGL context for Load(), Clear() and Draw() (textures are GL objects).

#include <cstdint>
#include <string>
#include <unordered_map>

#include <mdlpp/mdlpp.h>

// Everything about a resolved material that affects how it's drawn: the base texture plus
// the VMT $params that change render state.
struct MaterialGpuData {
	unsigned int textureId = 0;      // GL texture name, 0 if no texture could be loaded
	bool translucent = false;        // $translucent
	bool additive = false;           // $additive
	bool alphaTest = false;          // $alphatest
	float alphaTestReference = 0.7f; // $alphatestreference (Source's default)
	bool noCull = false;             // $nocull / $noculling
	float color[3] = { 1.0f, 1.0f, 1.0f }; // $color / $color2 tint
};

class ModelRenderer {
public:
	ModelRenderer() = default;
	ModelRenderer(const ModelRenderer&) = delete; // Owns GL textures
	ModelRenderer& operator=(const ModelRenderer&) = delete;

	// Loads and bakes a model by its indexed path (e.g. "models/props/foo.mdl").
	// On failure, returns false, GetLastError() says why, and nothing is loaded.
	bool Load(const std::string& modelPath);

	// Unloads the current model and frees its textures.
	void Clear();

	// Draws the loaded model at the origin. The caller sets up the projection and
	// modelview matrices.
	void Draw() const;

	bool IsLoaded() const { return loaded; }
	const std::string& GetLastError() const { return lastError; }

	// Number of skin families the loaded model has (0 if nothing is loaded).
	int GetSkinCount() const;

	// The skin family being rendered. Skin 0 is the model's default skin.
	int GetCurrentSkin() const { return currentSkin; }

	// Selects which skin family to render. Out-of-range values are clamped.
	void SetSkin(int skin);

private:
	void ReleaseTextures();
	void ComputeBounds();
	const MaterialGpuData& GetOrLoadMaterialData(int32_t materialIndex) const;
	MaterialGpuData LoadMaterialDataFromDisk(int32_t materialIndex) const;

	mdlpp::StudioModel studioModel;
	mdlpp::BakedModel bakedModel;
	bool loaded = false;
	std::string lastError;
	int currentSkin = 0;

	// Bounding info of the loaded model.
	float centerX = 0.0f, centerY = 0.0f, centerZ = 0.0f;
	float boundingRadius = 1.0f;

	// Resolved materials, keyed by material index. Filled lazily while drawing.
	mutable std::unordered_map<int32_t, MaterialGpuData> materialData;
};
