#pragma once
#include <string>
#include <vector>
#include <map>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cstring> 

// SourcePP VPK library header
#include <vpkpp/vpkpp.h>

using namespace std;
namespace fs = filesystem;

class SourceAssetManager {
public:
    struct SearchPath {
        fs::path path;
        bool isVpk = false;
        bool isWildcard = false;
    };

    // Dictionary to remember exactly which VPK or folder a model belongs to
    static inline map<string, string> ModelLocations;

    // Cache of skin counts per model path, so repeated lookups (e.g. per-frame UI rendering)
    // don't re-open the same file or VPK entry over and over.
    static inline map<string, int> SkinCountCache;

    static vector<string> IndexGameModels(const string& gameRootPath) {
        ModelLocations.clear(); // Clear previous game data
        SkinCountCache.clear(); // Invalidate cached skin counts from the previous index

        fs::path gameinfoFolder;
        fs::path gameinfoFile;

        try {
            for (const auto& entry : fs::directory_iterator(gameRootPath, fs::directory_options::skip_permission_denied)) {
                if (entry.is_directory()) {
                    fs::path target = entry.path() / "gameinfo.txt";
                    if (fs::exists(target)) {
                        gameinfoFolder = entry.path();
                        gameinfoFile = target;
                        break;
                    }
                }
            }
        }
        catch (const std::exception& e) {
            cerr << "[AssetManager] Error locating gameinfo.txt in " << gameRootPath << ": " << e.what() << '\n';
            return {};
        }

        if (gameinfoFile.empty()) return {};

        vector<SearchPath> searchPaths = ParseGameInfo(gameinfoFile, gameRootPath, gameinfoFolder);

        for (const auto& sp : searchPaths) {
            try {
                if (sp.isWildcard) {
                    if (fs::exists(sp.path) && fs::is_directory(sp.path)) {
                        for (const auto& subEntry : fs::directory_iterator(sp.path, fs::directory_options::skip_permission_denied)) {
                            if (subEntry.is_directory()) {
                                ScanDirectoryForModels(subEntry.path());
                            }
                            else if (subEntry.path().extension() == ".vpk") {
                                ScanVpkForModels(subEntry.path());
                            }
                        }
                    }
                }
                else if (sp.isVpk) {
                    ScanVpkForModels(sp.path);
                }
                else {
                    ScanDirectoryForModels(sp.path);
                }
            }
            catch (const std::exception& e) {
                cerr << "[AssetManager] Error scanning search path " << sp.path << ": " << e.what() << '\n';
            }
            catch (...) {
                cerr << "[AssetManager] Unknown error scanning search path " << sp.path << "!\n";
            }
        }

        // Return just the model paths for the UI
        vector<string> keys;
        for (const auto& [mdl, path] : ModelLocations) {
            keys.push_back(mdl);
        }
        return keys;
    }

    static int GetSkinCount(const string& modelPath) {
        auto cacheIt = SkinCountCache.find(modelPath);
        if (cacheIt != SkinCountCache.end()) return cacheIt->second;

        auto it = ModelLocations.find(modelPath);
        if (it == ModelLocations.end()) return -1; // Model not found in index

        string sourcePath = it->second;

        if (sourcePath.ends_with(".vpk")) {
            try {
                // Read directly from the VPK archive
                auto vpk = vpkpp::VPK::open(sourcePath);
                if (!vpk) return -1;

                // FIX: vpkpp is smart! Just ask it to read the string path directly.
                // No manual map searching needed.
                auto data = vpk->readEntry(modelPath);

                if (data.has_value() && data->size() >= 228) {
                    int skinFamilies = 0;
                    // 3 arguments: destination, source pointer, size
                    memcpy(&skinFamilies, data->data() + 224, 4);
                    SkinCountCache[modelPath] = skinFamilies;
                    return skinFamilies;
                }
            }
            catch (const std::exception& e) {
                cerr << "[AssetManager] Error reading skin count from VPK " << sourcePath << ": " << e.what() << '\n';
                return -1;
            }
            catch (...) {
                cerr << "[AssetManager] Unknown error reading skin count from VPK " << sourcePath << "!\n";
                return -1;
            }
        }
        else {
            // Read from loose file on disk
            fs::path fullPath = fs::path(sourcePath) / modelPath;
            ifstream stream(fullPath, ios::binary);
            if (stream.is_open()) {
                stream.seekg(224); // Jump straight to numskinfamilies
                int skinFamilies = 0;
                if (stream.read(reinterpret_cast<char*>(&skinFamilies), 4)) {
                    SkinCountCache[modelPath] = skinFamilies;
                    return skinFamilies;
                }
            }
        }
        return -1; // Failed to read
    }

    // Locates and reads the raw bytes for a model's .mdl, .vtx and .vvd companion files,
    // whether the model lives inside a VPK archive or as loose files on disk.
    // Returns false (with a human-readable reason in errorOut) if anything couldn't be found/read.
    static bool LoadStudioModelBytes(const string& modelPath,
                                      vector<std::byte>& mdlOut,
                                      vector<std::byte>& vtxOut,
                                      vector<std::byte>& vvdOut,
                                      string& errorOut) {
        auto it = ModelLocations.find(modelPath);
        if (it == ModelLocations.end()) {
            errorOut = "Model not found in index: " + modelPath;
            return false;
        }

        const string sourcePath = it->second;
        if (!modelPath.ends_with(".mdl")) {
            errorOut = "Not a .mdl path: " + modelPath;
            return false;
        }

        const string base = modelPath.substr(0, modelPath.size() - 4); // strip ".mdl"
        const string vvdPath = base + ".vvd";
        // VTX files can be compiled for different renderers; try the common ones in order.
        const vector<string> vtxCandidates = { base + ".dx90.vtx", base + ".dx80.vtx", base + ".sw.vtx" };

        if (sourcePath.ends_with(".vpk")) {
            try {
                auto vpk = vpkpp::VPK::open(sourcePath);
                if (!vpk) {
                    errorOut = "Failed to open VPK: " + sourcePath;
                    return false;
                }

                auto mdlData = vpk->readEntry(modelPath);
                if (!mdlData.has_value()) {
                    errorOut = "Failed to read .mdl entry from VPK: " + modelPath;
                    return false;
                }
                mdlOut = std::move(*mdlData);

                bool foundVtx = false;
                for (const auto& candidate : vtxCandidates) {
                    auto vtxData = vpk->readEntry(candidate);
                    if (vtxData.has_value()) {
                        vtxOut = std::move(*vtxData);
                        foundVtx = true;
                        break;
                    }
                }
                if (!foundVtx) {
                    errorOut = "Could not find a .vtx companion file for: " + modelPath;
                    return false;
                }

                auto vvdData = vpk->readEntry(vvdPath);
                if (!vvdData.has_value()) {
                    errorOut = "Failed to read .vvd entry from VPK: " + vvdPath;
                    return false;
                }
                vvdOut = std::move(*vvdData);
            }
            catch (const std::exception& e) {
                errorOut = "Error reading from VPK " + sourcePath + ": " + e.what();
                return false;
            }
            catch (...) {
                errorOut = "Unknown error reading from VPK " + sourcePath;
                return false;
            }
        }
        else {
            fs::path root = sourcePath;

            if (!ReadFileBytes(root / modelPath, mdlOut)) {
                errorOut = "Failed to read .mdl file: " + (root / modelPath).string();
                return false;
            }

            bool foundVtx = false;
            for (const auto& candidate : vtxCandidates) {
                if (ReadFileBytes(root / candidate, vtxOut)) {
                    foundVtx = true;
                    break;
                }
            }
            if (!foundVtx) {
                errorOut = "Could not find a .vtx companion file for: " + modelPath;
                return false;
            }

            if (!ReadFileBytes(root / vvdPath, vvdOut)) {
                errorOut = "Failed to read .vvd file: " + (root / vvdPath).string();
                return false;
            }
        }

        errorOut.clear();
        return true;
    }

private:
    static bool ReadFileBytes(const fs::path& path, vector<std::byte>& out) {
        if (!fs::exists(path)) return false;
        ifstream stream(path, ios::binary | ios::ate);
        if (!stream.is_open()) return false;

        std::streamsize size = stream.tellg();
        if (size <= 0) return false;
        stream.seekg(0, ios::beg);

        out.resize(static_cast<size_t>(size));
        if (!stream.read(reinterpret_cast<char*>(out.data()), size)) return false;
        return true;
    }


    static vector<SearchPath> ParseGameInfo(const fs::path& gameinfoPath, const fs::path& engineRoot, const fs::path& gameinfoFolder) {
        vector<SearchPath> resolvedPaths;
        ifstream file(gameinfoPath);
        if (!file.is_open()) return resolvedPaths;

        string line;
        bool insideFileSystem = false, insideSearchPaths = false;

        while (getline(file, line)) {
            string cleanLine = line;
            size_t commentPos = cleanLine.find("//");
            if (commentPos != string::npos) cleanLine = cleanLine.substr(0, commentPos);

            if (cleanLine.find("FileSystem") != string::npos) insideFileSystem = true;
            if (insideFileSystem && cleanLine.find("SearchPaths") != string::npos) insideSearchPaths = true;

            if (insideSearchPaths) {
                if (cleanLine.find("}") != string::npos && cleanLine.find("{") == string::npos) break;
                size_t firstQuote = cleanLine.find_first_of("\"");
                size_t lastQuote = cleanLine.find_last_of("\"");
                string rawPath;

                if (firstQuote != string::npos && lastQuote != string::npos && firstQuote != lastQuote) {
                    rawPath = cleanLine.substr(firstQuote + 1, lastQuote - firstQuote - 1);
                }
                else {
                    istringstream iss(cleanLine);
                    string key, val;
                    if (iss >> key >> val) rawPath = val;
                }

                if (rawPath.empty()) continue;

                string resolved = rawPath;
                size_t macroGameInfo = resolved.find("|gameinfo_path|");
                if (macroGameInfo != string::npos) resolved.replace(macroGameInfo, 15, gameinfoFolder.string() + "/");

                size_t macroEngine = resolved.find("|all_source_engine_paths|");
                if (macroEngine != string::npos) resolved.replace(macroEngine, 25, engineRoot.string() + "/");

                fs::path finalPath = resolved;
                if (finalPath.is_relative()) finalPath = engineRoot / finalPath;

                SearchPath sp;
                sp.path = finalPath.lexically_normal();

                if (sp.path.filename().string() == "*") {
                    sp.isWildcard = true;
                    sp.path = sp.path.parent_path();
                }
                else if (sp.path.extension() == ".vpk") {
                    sp.isVpk = true;
                }

                resolvedPaths.push_back(sp);
            }
        }
        return resolvedPaths;
    }

    // Modified to write to the dictionary instead of a set
    static void ScanDirectoryForModels(const fs::path& dirPath) {
        // Source engine search paths listed as plain folders (e.g. "garrysmod") are also
        // implicitly backed by a sibling "<foldername>_dir.vpk" pack file when present,
        // even though gameinfo.txt never mentions it explicitly. Garry's Mod ships most of
        // its base content this way, so it must be scanned too or its models go undetected.
        fs::path vpkCompanion = dirPath.parent_path() / (dirPath.filename().string() + "_dir.vpk");
        if (fs::exists(vpkCompanion)) {
            ScanVpkForModels(vpkCompanion);
        }

        if (!fs::exists(dirPath) || !fs::is_directory(dirPath)) return;
        try {
            for (const auto& entry : fs::recursive_directory_iterator(dirPath, fs::directory_options::skip_permission_denied)) {
                if (entry.is_regular_file() && entry.path().extension() == ".mdl") {
                    string relativePath = fs::relative(entry.path(), dirPath).generic_string();
                    if (relativePath.rfind("models/", 0) == 0) {
                        ModelLocations[relativePath] = dirPath.string(); // Save where we found it
                    }
                }
            }
        }
        catch (const fs::filesystem_error& e) {
            // Catch specific filesystem errors (e.g., paths too long, locked files)
            cerr << "[AssetManager] Filesystem Error scanning " << dirPath << ": " << e.what() << '\n';
        }
        catch (const std::exception& e) {
            // Catch any other standard C++ exceptions
            cerr << "[AssetManager] Standard Error scanning " << dirPath << ": " << e.what() << '\n';
        }
        catch (...) {
            // Final fallback in case the error is unrecognized
            cerr << "[AssetManager] Unknown critical error while scanning " << dirPath << "!\n";
        }
    }

    // Modified to write to the dictionary instead of a set
    static void ScanVpkForModels(const fs::path& vpkPath) {
        if (!fs::exists(vpkPath)) return;
        try {
            auto vpk = vpkpp::VPK::open(vpkPath.string());
            if (!vpk) return;

            const auto& baked = vpk->getBakedEntries();
            for (auto it = baked.begin(); it != baked.end(); ++it) {

                // FIX: In tsl::htrie_map, the path is stored as the map key!
                string fullEntryPath = string(it.key());

                if (fs::path(fullEntryPath).extension() == ".mdl") {
                    if (fullEntryPath.rfind("models/", 0) == 0) {
                        ModelLocations[fullEntryPath] = vpkPath.string(); // Save where we found it
                    }
                }
            }
        }
        catch (const std::exception& e) {
            cerr << "[AssetManager] Error reading VPK " << vpkPath << ": " << e.what() << '\n';
        }
        catch (...) {
            cerr << "[AssetManager] Unknown error reading VPK " << vpkPath << "!\n";
        }
    }
};