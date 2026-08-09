#pragma once
#include <string>
#include <vector>
#include <map>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <cstring> 
#include <mutex>
#include <functional>
#include <chrono>
#include <thread>
#include <future>

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

    // Guards ModelLocations/SkinCountCache, since IndexGameModels is meant to be run on a
    // background thread (so the UI thread never blocks/freezes while scanning a game's
    // assets) while the UI thread concurrently reads them (model list, skin counts, preview).
    static inline mutex IndexMutex;

    // gameinfo.txt often references a VPK by its "logical" name (e.g. "garrysmod.vpk"),
    // while on disk the actual multi-chunk archive is named "<name>_dir.vpk" (with the
    // data itself split into "<name>_000.vpk", etc). The Source engine transparently
    // resolves this at mount time; we need to do the same or the VPK is never found.
    static fs::path ResolveActualVpkPath(const fs::path& vpkPath) {
        if (fs::exists(vpkPath)) return vpkPath;

        fs::path dirVariant = vpkPath.parent_path() / (vpkPath.stem().string() + "_dir" + vpkPath.extension().string());
        if (fs::exists(dirVariant)) return dirVariant;

        return vpkPath; // Not found either way; let the caller fail gracefully
    }

    // Safe to call from a background thread: only builds a local index and swaps it into
    // the shared ModelLocations/SkinCountCache under a lock at the very end, so the UI
    // thread is never blocked for the whole (potentially slow) duration of the scan.
    static vector<string> IndexGameModels(const string& gameRootPath) {
        map<string, string> localModelLocations; // Built up locally, published atomically at the end

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
            PublishIndex(localModelLocations);
            return {};
        }

        if (gameinfoFile.empty()) {
            PublishIndex(localModelLocations);
            return {};
        }

        auto indexStartTime = std::chrono::steady_clock::now();

        vector<SearchPath> searchPaths = ParseGameInfo(gameinfoFile, gameRootPath, gameinfoFolder);

        // Re-scanning every VPK/model folder from scratch
        // directory index) is by far the most expensive part of indexing, and the result
        // never changes unless the game's files themselves change. Try a disk cache first,
        // keyed by gameRootPath and validated against the search paths' last-write times;
        // this turns every re-selection of an already-indexed game into a near-instant load.
        fs::path cacheFile = GetIndexCachePath(gameRootPath);
        uint64_t fingerprint = ComputeSearchPathsFingerprint(gameinfoFile, searchPaths);

        if (LoadIndexCache(cacheFile, fingerprint, localModelLocations)) {
            vector<string> cachedKeys;
            cachedKeys.reserve(localModelLocations.size());
            for (const auto& [mdl, path] : localModelLocations) {
                cachedKeys.push_back(mdl);
            }
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - indexStartTime).count();
            cerr << "[AssetManager][Timing] Loaded from disk cache in " << elapsedMs << " ms (" << localModelLocations.size() << " models)\n";
            PublishIndex(localModelLocations);
            return cachedKeys;
        }

        // Each search path (a VPK archive or a "models/" folder) is completely independent
        // of the others, and opening/parsing a big VPK's directory index is the expensive
        // part of scanning (e.g. tf2_misc.vpk, hl2_misc.vpk, platform_misc.vpk all get
        // scanned back-to-back for TF2). Flatten every independent unit of work into its
        // own task and run them across a thread pool, instead of doing them one at a time.
        vector<std::function<map<string, string>()>> tasks;

        for (const auto& sp : searchPaths) {
            if (sp.isWildcard) {
                if (fs::exists(sp.path) && fs::is_directory(sp.path)) {
                    try {
                        for (const auto& subEntry : fs::directory_iterator(sp.path, fs::directory_options::skip_permission_denied)) {
                            fs::path subPath = subEntry.path();
                            bool isDir = subEntry.is_directory();
                            // Multi-chunk VPKs are split into "<name>_dir.vpk" (the directory
                            // index we actually need) plus "<name>_000.vpk", "<name>_001.vpk",
                            // etc (raw data chunks with no index of their own). Opening those
                            // chunk files directly is pointless and wasteful - only "_dir.vpk"
                            // archives ever need to be scanned.
                            bool isVpk = subPath.extension() == ".vpk" && subPath.stem().string().ends_with("_dir");
                            if (isDir) {
                                tasks.push_back([subPath]() {
                                    map<string, string> local;
                                    ScanDirectoryForModels(subPath, local);
                                    return local;
                                });
                            }
                            else if (isVpk) {
                                tasks.push_back([subPath]() {
                                    map<string, string> local;
                                    ScanVpkForModels(subPath, local);
                                    return local;
                                });
                            }
                        }
                    }
                    catch (const std::exception& e) {
                        cerr << "[AssetManager] Error scanning wildcard path " << sp.path << ": " << e.what() << '\n';
                    }
                }
            }
            else if (sp.isVpk) {
                fs::path vpkPath = sp.path;
                tasks.push_back([vpkPath]() {
                    map<string, string> local;
                    ScanVpkForModels(vpkPath, local);
                    return local;
                });
            }
            else {
                fs::path dirPath = sp.path;
                tasks.push_back([dirPath]() {
                    map<string, string> local;
                    ScanDirectoryForModels(dirPath, local);
                    return local;
                });
            }
        }

        // Run tasks across a small thread pool (bounded by hardware concurrency), merging
        // each finished task's local map into localModelLocations as it completes.
        unsigned int hwThreads = std::thread::hardware_concurrency();
        size_t poolSize = std::max(1u, std::min(hwThreads == 0 ? 4u : hwThreads, static_cast<unsigned int>(tasks.size())));

        std::mutex taskMutex;
        size_t nextTask = 0;
        std::mutex mergeMutex;

        auto worker = [&]() {
            while (true) {
                size_t myIndex;
                {
                    std::lock_guard<std::mutex> lock(taskMutex);
                    if (nextTask >= tasks.size()) return;
                    myIndex = nextTask++;
                }
                try {
                    map<string, string> result = tasks[myIndex]();
                    std::lock_guard<std::mutex> lock(mergeMutex);
                    for (auto& [mdl, path] : result) {
                        localModelLocations[mdl] = std::move(path);
                    }
                }
                catch (const std::exception& e) {
                    cerr << "[AssetManager] Error running scan task: " << e.what() << '\n';
                }
                catch (...) {
                    cerr << "[AssetManager] Unknown error running scan task!\n";
                }
            }
        };

        cerr << "[AssetManager][Timing] Running " << tasks.size() << " scan tasks across " << poolSize << " threads...\n";

        vector<std::thread> workers;
        workers.reserve(poolSize);
        for (size_t i = 0; i < poolSize; ++i) {
            workers.emplace_back(worker);
        }
        for (auto& w : workers) w.join();

        auto scanElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - indexStartTime).count();
        cerr << "[AssetManager][Timing] All scan tasks finished in " << scanElapsedMs << " ms\n";

        // Return just the model paths for the UI
        vector<string> keys;
        keys.reserve(localModelLocations.size());
        for (const auto& [mdl, path] : localModelLocations) {
            keys.push_back(mdl);
        }

        SaveIndexCache(cacheFile, fingerprint, localModelLocations);

        PublishIndex(localModelLocations);
        return keys;
    }

    // Returns a stable per-game cache file path under the system temp directory, derived
    // from a hash of the game's root path (so different games/libraries never collide).
    static fs::path GetIndexCachePath(const string& gameRootPath) {
        hash<string> hasher;
        size_t h = hasher(gameRootPath);
        fs::path cacheDir = fs::temp_directory_path() / "SprinkleScriptMaker" / "AssetIndexCache";
        error_code ec;
        fs::create_directories(cacheDir, ec);
        ostringstream name;
        name << hex << h << ".idx";
        return cacheDir / name.str();
    }

    // Cheap fingerprint of everything that could invalidate a cached index: gameinfo.txt's
    // own last-write time, plus each resolved search path's last-write time (VPK archives
    // and model directories alike). Any mod update, VPK repack, or new addon touches at
    // least one of these, so the cache stays correct without hashing file contents.
    static uint64_t ComputeSearchPathsFingerprint(const fs::path& gameinfoFile, const vector<SearchPath>& searchPaths) {
        uint64_t fp = 1469598103934665603ULL; // FNV offset basis
        auto mix = [&fp](uint64_t value) {
            fp ^= value;
            fp *= 1099511628211ULL; // FNV prime
        };

        std::error_code ec;
        auto mixTime = [&](const fs::path& p) {
            auto t = fs::last_write_time(p, ec);
            if (!ec) {
                mix(static_cast<uint64_t>(t.time_since_epoch().count()));
            }
        };

        mixTime(gameinfoFile);
        for (const auto& sp : searchPaths) {
            mixTime(sp.path);
            fs::path vpkCompanion = sp.path.parent_path() / (sp.path.filename().string() + "_dir.vpk");
            mixTime(vpkCompanion);
        }
        return fp;
    }

    // Simple line-based cache format: first line is the fingerprint, followed by
    // "<modelPath>\t<sourcePath>" pairs. Rejects the cache outright on any mismatch/error.
    static bool LoadIndexCache(const fs::path& cacheFile, uint64_t expectedFingerprint, map<string, string>& out) {
        std::ifstream in(cacheFile);
        if (!in.is_open()) return false;

        string headerLine;
        if (!std::getline(in, headerLine)) return false;

        uint64_t storedFingerprint = 0;
        try {
            storedFingerprint = std::stoull(headerLine);
        }
        catch (...) {
            return false;
        }
        if (storedFingerprint != expectedFingerprint) return false;

        map<string, string> loaded;
        string line;
        while (std::getline(in, line)) {
            size_t tab = line.find('\t');
            if (tab == string::npos) continue;
            loaded[line.substr(0, tab)] = line.substr(tab + 1);
        }

        out = move(loaded);
        return true;
    }

    static void SaveIndexCache(const fs::path& cacheFile, uint64_t fingerprint, const map<string, string>& data) {
        ofstream out(cacheFile, ios::trunc);
        if (!out.is_open()) return;

        out << fingerprint << '\n';
        for (const auto& [mdl, path] : data) {
            out << mdl << '\t' << path << '\n';
        }
    }

    // Returns a thread-safe snapshot copy of ModelLocations, suitable for the UI thread to
    // iterate over (e.g. drawing the model list) without racing a background index update.
    static map<string, string> GetModelLocationsSnapshot() {
        lock_guard<mutex> lock(IndexMutex);
        return ModelLocations;
    }

    static int GetSkinCount(const string& modelPath) {
        string sourcePath;
        {
            lock_guard<mutex> lock(IndexMutex);
            auto cacheIt = SkinCountCache.find(modelPath);
            if (cacheIt != SkinCountCache.end()) return cacheIt->second;

            auto it = ModelLocations.find(modelPath);
            if (it == ModelLocations.end()) return -1; // Model not found in index
            sourcePath = it->second;
        }
        if (sourcePath.ends_with(".vpk")) {
            try {
                // Read directly from the VPK archive
                auto vpk = vpkpp::VPK::open(ResolveActualVpkPath(sourcePath).string());
                if (!vpk) return -1;

                // FIX: vpkpp is smart! Just ask it to read the string path directly.
                // No manual map searching needed.
                auto data = vpk->readEntry(modelPath);

                if (data.has_value() && data->size() >= 228) {
                    int skinFamilies = 0;
                    // 3 arguments: destination, source pointer, size
                    memcpy(&skinFamilies, data->data() + 224, 4);
                    lock_guard<mutex> lock(IndexMutex);
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
                    lock_guard<mutex> lock(IndexMutex);
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
        string sourcePath;
        {
            lock_guard<mutex> lock(IndexMutex);
            auto it = ModelLocations.find(modelPath);
            if (it == ModelLocations.end()) {
                errorOut = "Model not found in index: " + modelPath;
                return false;
            }
            sourcePath = it->second;
        }
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
                auto vpk = vpkpp::VPK::open(ResolveActualVpkPath(sourcePath).string());
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
    static void ScanDirectoryForModels(const fs::path& dirPath, map<string, string>& out) {
        auto scanDirStartTime = std::chrono::steady_clock::now();
        // Source engine search paths listed as plain folders (e.g. "garrysmod") are also
        // implicitly backed by a sibling "<foldername>_dir.vpk" pack file when present,
        // even though gameinfo.txt never mentions it explicitly. Garry's Mod ships most of
        // its base content this way, so it must be scanned too or its models go undetected.
        fs::path vpkCompanion = dirPath.parent_path() / (dirPath.filename().string() + "_dir.vpk");
        if (fs::exists(vpkCompanion)) {
            ScanVpkForModels(vpkCompanion, out);
        }

        if (!fs::exists(dirPath) || !fs::is_directory(dirPath)) return;

        // Source engine mod folders always keep .mdl files under a "models" subfolder.
        // Recursing from the search path root would also walk huge, irrelevant trees
        // (materials/, sound/, maps/, particles/, addons content, shared "sourceengine"
        // textures, etc), which is the main cause of slow startup/game-switch indexing.
        // Restricting the walk to just "<dirPath>/models" skips all of that for free.
        fs::path modelsDir = dirPath / "models";
        if (!fs::exists(modelsDir) || !fs::is_directory(modelsDir)) return;

        try {
            for (const auto& entry : fs::recursive_directory_iterator(modelsDir, fs::directory_options::skip_permission_denied)) {
                if (entry.is_regular_file() && entry.path().extension() == ".mdl") {
                    string relativePath = "models/" + fs::relative(entry.path(), modelsDir).generic_string();
                    out[relativePath] = dirPath.string(); // Save where we found it
                }
            }
        }
        catch (const fs::filesystem_error& e) {
            // Catch specific filesystem errors (e.g., paths too long, locked files)
            cerr << "[AssetManager] Filesystem Error scanning " << modelsDir << ": " << e.what() << '\n';
        }
        catch (const std::exception& e) {
            // Catch any other standard C++ exceptions
            cerr << "[AssetManager] Standard Error scanning " << modelsDir << ": " << e.what() << '\n';
        }
        catch (...) {
            // Final fallback in case the error is unrecognized
            cerr << "[AssetManager] Unknown critical error while scanning " << modelsDir << "!\n";
        }

        auto scanDirElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - scanDirStartTime).count();
        if (scanDirElapsedMs > 5) {
            cerr << "[AssetManager][Timing] Directory " << dirPath << ": " << scanDirElapsedMs << "ms\n";
        }
    }

    // Modified to write to the dictionary instead of a set
    static void ScanVpkForModels(const fs::path& vpkPath, map<string, string>& out) {
        auto startTime = std::chrono::steady_clock::now();
        fs::path actualVpkPath = ResolveActualVpkPath(vpkPath);
        if (!fs::exists(actualVpkPath)) return;

        // Valve's naming convention splits shared engine content into VPKs by asset type
        // (e.g. "hl2_textures.vpk", "hl2_sound_vo_english.vpk", "platform_misc.vpk").
        // Texture/sound-only archives never contain .mdl files, and opening/parsing a VPK's
        // full directory index is the expensive part of scanning - skip them entirely
        // instead of paying that cost just to find nothing.
        string lowerName = actualVpkPath.filename().string();
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), [](unsigned char c) { return std::tolower(c); });
        if (lowerName.find("_textures") != string::npos || lowerName.find("_sound") != string::npos) {
            return;
        }

        try {
            auto openStart = std::chrono::steady_clock::now();
            auto vpk = vpkpp::VPK::open(actualVpkPath.string());
            auto openElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - openStart).count();
            if (!vpk) return;

            const auto& baked = vpk->getBakedEntries();
            for (auto it = baked.begin(); it != baked.end(); ++it) {

                // FIX: In tsl::htrie_map, the path is stored as the map key!
                string fullEntryPath = string(it.key());

                if (fs::path(fullEntryPath).extension() == ".mdl") {
                    if (fullEntryPath.rfind("models/", 0) == 0) {
                        out[fullEntryPath] = actualVpkPath.string(); // Save where we found it
                    }
                }
            }

            auto totalElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count();
            cerr << "[AssetManager][Timing] VPK " << actualVpkPath.filename().string() << ": open=" << openElapsedMs << "ms, total=" << totalElapsedMs << "ms\n";
        }
        catch (const std::exception& e) {
            cerr << "[AssetManager] Error reading VPK " << actualVpkPath << ": " << e.what() << '\n';
        }
        catch (...) {
            cerr << "[AssetManager] Unknown error reading VPK " << actualVpkPath << "!\n";
        }
    }

    // Atomically publishes a freshly-built local index as the new shared ModelLocations,
    // invalidating the skin count cache (which belonged to whatever was indexed before).
    static void PublishIndex(map<string, string>& localModelLocations) {
        std::lock_guard<std::mutex> lock(IndexMutex);
        ModelLocations = std::move(localModelLocations);
        SkinCountCache.clear();
    }
};