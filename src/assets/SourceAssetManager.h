#pragma once

// Indexes and reads the assets (models, materials, textures) of a Source Engine game,
// from loose folders and VPK archives, following the game's gameinfo.txt search paths.
//
// All state is shared process-wide. IndexGameModels() is meant to run on a background
// thread while the UI thread calls the other functions; access is synchronized internally.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace SourceAssetManager {

	// Bumped by the UI thread every time the selected game changes. IndexGameModels()
	// compares it against the generation it was started with, and stops (without
	// touching shared state) as soon as a newer indexing run has been requested.
	extern std::atomic<std::uint64_t> IndexGeneration;

	// Scans every model of the game installed at gameRootPath and publishes the result as
	// the new shared index. Results are cached on disk and reused while the game's files
	// don't change. Returns the indexed model paths (e.g. "models/props/foo.mdl"), or an
	// empty list if the run was superseded by a newer one.
	//
	// myGeneration is the value of IndexGeneration the caller set before starting this run.
	std::vector<std::string> IndexGameModels(const std::string& gameRootPath, std::uint64_t myGeneration);

	// Thread-safe copy of the current index: model path -> VPK archive or folder it lives in.
	std::map<std::string, std::string> GetModelLocationsSnapshot();

	// Number of skin families of an indexed model (cached), or -1 if it can't be read.
	int GetSkinCount(const std::string& modelPath);

	// Reads an indexed model's .mdl and its .vtx/.vvd companion files.
	// Returns false with a human-readable reason in errorOut if anything is missing.
	bool LoadStudioModelBytes(const std::string& modelPath,
	                          std::vector<std::byte>& mdlOut,
	                          std::vector<std::byte>& vtxOut,
	                          std::vector<std::byte>& vvdOut,
	                          std::string& errorOut);

	// Reads any asset by its game-relative path (e.g. "materials/props/foo.vtf") by
	// searching the last indexed game's search paths. Returns false if it isn't found.
	bool LoadAssetBytes(const std::string& relPath, std::vector<std::byte>& out, std::string& errorOut);

} // namespace SourceAssetManager
