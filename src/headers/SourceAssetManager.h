#pragma once
#include <string>
#include <vector>
#include <map>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <cstring> 
#include <mutex>
#include <functional>
#include <chrono>
#include <thread>
#include <future>
#include <memory>
#include <atomic>

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

    // The search paths discovered from the last indexed game's gameinfo.txt. Reused for
    // generic asset lookups (textures/materials) that aren't part of the model index.
    static inline vector<SearchPath> GameSearchPaths;

    // Cache of skin counts per model path, so repeated lookups (e.g. per-frame UI rendering)
    // don't re-open the same file or VPK entry over and over.
    static inline map<string, int> SkinCountCache;

    // Cache of already-opened VPK archives, keyed by resolved archive path. Opening a VPK
    // re-parses its whole directory index, which is expensive; without this cache, scrolling
    // the model list into rows that share the same (large) VPK but haven't been queried yet
    // reopens/re-parses that same archive once per model, causing the freeze reported when
    // scrolling reaches previously-unseen rows.
    static inline map<string, shared_ptr<vpkpp::PackFile>> OpenVpkCache;
    static inline mutex OpenVpkMutex;

    // Guards ModelLocations/SkinCountCache, since IndexGameModels is meant to be run on a
    // background thread (so the UI thread never blocks/freezes while scanning a game's
    // assets) while the UI thread concurrently reads them (model list, skin counts, preview).
    static inline mutex IndexMutex;

    // Bumped by the UI thread every time the selected game changes. IndexGameModels()
    // captures the value in effect when it started and checks it periodically (between
    // scan tasks); if it no longer matches, an indexing run for a since-abandoned game is
    // in flight and should stop touching shared state / doing further work instead of
    // racing with (and potentially clobbering) the newer game's own indexing run.
    static inline atomic<uint64_t> IndexGeneration{0};

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

    // Returns a shared, cached handle to an already-open VPK archive, opening (and parsing)
    // it only the first time a given archive path is requested.
    static shared_ptr<vpkpp::PackFile> GetOrOpenVpk(const string& actualVpkPath) {
        {
            lock_guard<mutex> lock(OpenVpkMutex);
            auto it = OpenVpkCache.find(actualVpkPath);
            if (it != OpenVpkCache.end()) return it->second;
        }

        auto vpk = vpkpp::VPK::open(actualVpkPath);
        shared_ptr<vpkpp::PackFile> shared = vpk ? shared_ptr<vpkpp::PackFile>(std::move(vpk)) : nullptr;

        lock_guard<mutex> lock(OpenVpkMutex);
        OpenVpkCache[actualVpkPath] = shared;
        return shared;
    }

    // Safe to call from a background thread: only builds a local index and swaps it into
    // the shared ModelLocations/SkinCountCache under a lock at the very end, so the UI
    // thread is never blocked for the whole (potentially slow) duration of the scan.
    //
    // myGeneration should be the value of IndexGeneration captured by the caller right
    // before bumping it and starting this run (see main.cpp). If IndexGeneration no
    // longer matches myGeneration by the time we'd otherwise do meaningful work, the
    // selected game has changed again since this run started - bail out immediately
    // without touching shared state or doing further scanning, so an old game's scan
    // never overwrites the results of a newer one.
    static vector<string> IndexGameModels(const string& gameRootPath, uint64_t myGeneration) {
        map<string, string> localModelLocations; // Built up locally, published atomically at the end

        auto isStale = [&]() { return IndexGeneration.load() != myGeneration; };

        fs::path gameinfoFolder;
        fs::path gameinfoFile;

        try {
            for (const auto& entry : fs::directory_iterator(gameRootPath, fs::directory_options::skip_permission_denied)) {
                if (isStale()) return {};
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
            if (!isStale()) PublishIndex(localModelLocations);
            return {};
        }

        if (isStale()) return {};

        if (gameinfoFile.empty()) {
            PublishIndex(localModelLocations);
            return {};
        }

        auto indexStartTime = std::chrono::steady_clock::now();

        vector<SearchPath> searchPaths = ParseGameInfo(gameinfoFile, gameRootPath, gameinfoFolder);

        // Garry's Mod-specific extra sources: workshop/legacy addons (which aren't listed
        // in gameinfo.txt at all - the engine mounts them dynamically at runtime) and any
        // external Source Engine game paths mounted via cfg/mount.cfg. Both can contain
        // models that would otherwise never show up in the model list.
        AppendGModAddonSearchPaths(gameRootPath, searchPaths);
        AppendGModMountCfgSearchPaths(gameRootPath, searchPaths);

        if (isStale()) return {};

        {
            lock_guard<mutex> lock(IndexMutex);
            GameSearchPaths = searchPaths;
        }

        // Re-scanning every VPK/model folder from scratch
        // directory index) is by far the most expensive part of indexing, and the result
        // never changes unless the game's files themselves change. Try a disk cache first,
        // keyed by gameRootPath and validated against the search paths' last-write times;
        // this turns every re-selection of an already-indexed game into a near-instant load.
        fs::path cacheFile = GetIndexCachePath(gameRootPath);
        uint64_t fingerprint = ComputeSearchPathsFingerprint(gameinfoFile, searchPaths, gameRootPath);

        if (LoadIndexCache(cacheFile, fingerprint, localModelLocations)) {
            if (isStale()) return {};
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
                if (isStale()) return; // Selected game changed again; abandon remaining tasks for this stale run
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

        if (isStale()) {
            // A newer game was selected while this run was still scanning; discard our
            // (possibly incomplete) results instead of caching/publishing them, so we
            // don't pollute the disk cache or clobber the newer run's in-progress state.
            return {};
        }

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
    static uint64_t ComputeSearchPathsFingerprint(const fs::path& gameinfoFile, const vector<SearchPath>& searchPaths, const fs::path& gameRootPath = {}) {
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
        // Also cover GMod's mount.cfg and addons folder itself, so editing mount.cfg or
        // adding/removing an addon folder invalidates the cache even if none of the
        // resolved search paths' own timestamps happen to change.
        if (!gameRootPath.empty()) {
            mixTime(gameRootPath / "garrysmod" / "cfg" / "mount.cfg");
            mixTime(gameRootPath / "garrysmod" / "addons");
        }
        for (const auto& sp : searchPaths) {
            mixTime(sp.path);
            fs::path vpkCompanion = sp.path.parent_path() / (sp.path.filename().string() + "_dir.vpk");
            mixTime(vpkCompanion);
            // Cover every "*_dir.vpk" archive shipped directly inside the mod folder
            // itself (pak01_dir.vpk, pak02_dir.vpk, or any other naming some games use,
            // e.g. Alien Swarm: Reactive Drop), not just a fixed "pakNN" scheme.
            if (fs::exists(sp.path) && fs::is_directory(sp.path)) {
                std::error_code dirEc;
                for (const auto& entry : fs::directory_iterator(sp.path, fs::directory_options::skip_permission_denied, dirEc)) {
                    if (!entry.is_regular_file()) continue;
                    const fs::path& candidate = entry.path();
                    if (candidate.extension() == ".vpk" && candidate.stem().string().ends_with("_dir")) {
                        mixTime(candidate);
                    }
                }
            }
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
                // Read directly from the VPK archive (reusing an already-open handle rather
                // than reopening/re-parsing the whole archive for every model queried)
                auto vpk = GetOrOpenVpk(ResolveActualVpkPath(sourcePath).string());
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

    // Locates and reads the raw bytes of an arbitrary asset (e.g. a .vmt or .vtf material
    // file) by its game-relative path (e.g. "materials/props/foo.vtf"), searching every
    // known search path (loose folders and VPKs) discovered from the last-indexed game's
    // gameinfo.txt. Unlike LoadStudioModelBytes this isn't backed by a pre-built index,
    // since textures/materials aren't enumerated up front - so this walks the search
    // paths at call time. Returns false (with a reason in errorOut) if not found anywhere.
    static bool LoadAssetBytes(const string& relPath, vector<std::byte>& out, string& errorOut) {
        vector<SearchPath> searchPaths;
        {
            lock_guard<mutex> lock(IndexMutex);
            searchPaths = GameSearchPaths;
        }

        for (const auto& sp : searchPaths) {
            if (sp.isWildcard) {
                if (!fs::exists(sp.path) || !fs::is_directory(sp.path)) continue;
                try {
                    for (const auto& subEntry : fs::directory_iterator(sp.path, fs::directory_options::skip_permission_denied)) {
                        fs::path subPath = subEntry.path();
                        bool isVpk = subPath.extension() == ".vpk" && subPath.stem().string().ends_with("_dir");
                        if (subEntry.is_directory()) {
                            if (ReadFileBytes(subPath / relPath, out)) {
                                errorOut.clear();
                                return true;
                            }
                        }
                        else if (isVpk) {
                            auto vpk = GetOrOpenVpk(ResolveActualVpkPath(subPath).string());
                            if (vpk) {
                                auto data = vpk->readEntry(relPath);
                                if (data.has_value()) {
                                    out = std::move(*data);
                                    errorOut.clear();
                                    return true;
                                }
                            }
                        }
                    }
                }
                catch (const std::exception&) {
                    // Ignore and keep searching other search paths.
                }
            }
            else if (sp.isVpk) {
                auto vpk = GetOrOpenVpk(ResolveActualVpkPath(sp.path).string());
                if (vpk) {
                    auto data = vpk->readEntry(relPath);
                    if (data.has_value()) {
                        out = std::move(*data);
                        errorOut.clear();
                        return true;
                    }
                }
            }
            else {
                if (ReadFileBytes(sp.path / relPath, out)) {
                    errorOut.clear();
                    return true;
                }

                // Mirrors ScanDirectoryForModels(): plain-folder search paths (e.g.
                // "garrysmod") are often implicitly backed by a sibling
                // "<foldername>_dir.vpk" that gameinfo.txt never mentions explicitly.
                // Models found there are correctly indexed via ModelLocations (which
                // stores the VPK path directly), but without this same fallback here,
                // every material/texture lookup for those models would fail even
                // though the .vmt/.vtf physically lives inside that VPK.
                fs::path vpkCompanion = sp.path.parent_path() / (sp.path.filename().string() + "_dir.vpk");
                if (fs::exists(vpkCompanion)) {
                    auto vpk = GetOrOpenVpk(vpkCompanion.string());
                    if (vpk) {
                        auto data = vpk->readEntry(relPath);
                        if (data.has_value()) {
                            out = std::move(*data);
                            errorOut.clear();
                            return true;
                        }
                    }
                }

                // Newer Source games (Portal 2, L4D2, CS:GO, Alien Swarm, etc.) instead
                // pack their base content INSIDE the mod folder itself as one or more
                // "*_dir.vpk" archives (e.g. "portal2/pak01_dir.vpk"). Check those too.
                if (fs::exists(sp.path) && fs::is_directory(sp.path)) {
                    try {
                        for (const auto& subEntry : fs::directory_iterator(sp.path, fs::directory_options::skip_permission_denied)) {
                            if (!subEntry.is_regular_file()) continue;
                            const fs::path& candidate = subEntry.path();
                            if (candidate.extension() == ".vpk" && candidate.stem().string().ends_with("_dir")) {
                                auto vpk = GetOrOpenVpk(candidate.string());
                                if (vpk) {
                                    auto data = vpk->readEntry(relPath);
                                    if (data.has_value()) {
                                        out = std::move(*data);
                                        errorOut.clear();
                                        return true;
                                    }
                                }
                            }
                        }
                    }
                    catch (const std::exception&) {
                        // Ignore and keep searching other search paths.
                    }
                }
            }
        }

        errorOut = "Asset not found in any search path: " + relPath;
        return false;
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

    // Garry's Mod loads legacy/local addons (not Workshop-synced ones, which live in the
    // Steam Workshop content cache instead) from "<gmodRoot>/garrysmod/addons/<addon>/".
    // These are never listed in gameinfo.txt - the engine mounts every immediate
    // subfolder here dynamically at runtime - so without this, any model shipped inside
    // a legacy addon folder is invisible to the indexer even though the game itself can
    // see and load it fine. Only addon subfolders that actually contain a "models" (or
    // "materials") folder are treated as valid content sources, mirroring how the engine
    // decides an addon folder is worth mounting.
    static void AppendGModAddonSearchPaths(const fs::path& gameRootPath, vector<SearchPath>& searchPaths) {
        fs::path addonsDir = gameRootPath / "garrysmod" / "addons";
        if (!fs::exists(addonsDir) || !fs::is_directory(addonsDir)) return;

        try {
            for (const auto& entry : fs::directory_iterator(addonsDir, fs::directory_options::skip_permission_denied)) {
                if (!entry.is_directory()) continue;

                fs::path addonPath = entry.path();
                bool hasModels = fs::exists(addonPath / "models") && fs::is_directory(addonPath / "models");
                bool hasMaterials = fs::exists(addonPath / "materials") && fs::is_directory(addonPath / "materials");

                if (hasModels || hasMaterials) {
                    SearchPath sp;
                    sp.path = addonPath.lexically_normal();
                    searchPaths.push_back(sp);
                }
            }
        }
        catch (const std::exception& e) {
            cerr << "[AssetManager] Error scanning GMod addons folder " << addonsDir << ": " << e.what() << '\n';
        }
    }

    // Garry's Mod supports mounting other installed Source Engine games' content via
    // "<gmodRoot>/garrysmod/cfg/mount.cfg", a simple KeyValues-style text file:
    //
    //   "mountcfg"
    //   {
    //       "cstrike"   "C:\...\Counter-Strike Source Dedicated Server\cstrike"
    //       "tf"        "C:\mytf2server\tf"
    //   }
    //
    // Lines starting with "//" are comments (including the two commented-out example
    // lines Valve ships inside the block by default, which must be ignored - not treated
    // as real mounts). Every uncommented "key" "path" pair inside the block is an extra
    // folder GMod mounts at runtime, and needs to be scanned the same way any other
    // plain-folder search path is, so models from the mounted game appear in the list.
    static void AppendGModMountCfgSearchPaths(const fs::path& gameRootPath, vector<SearchPath>& searchPaths) {
        fs::path mountCfgPath = gameRootPath / "garrysmod" / "cfg" / "mount.cfg";
        ifstream file(mountCfgPath);
        if (!file.is_open()) return;

        string line;
        bool insideBlock = false;

        while (getline(file, line)) {
            // Trim leading whitespace so comment/brace detection isn't fooled by indentation.
            size_t firstNonSpace = line.find_first_not_of(" \t\r\n");
            if (firstNonSpace == string::npos) continue;
            string trimmed = line.substr(firstNonSpace);

            // Ignore full-line comments entirely - this also correctly skips the two
            // commented-out example mount lines Valve ships inside the block.
            if (trimmed.rfind("//", 0) == 0) continue;

            if (!insideBlock) {
                if (trimmed.find('{') != string::npos) insideBlock = true;
                continue;
            }

            if (trimmed.find('}') != string::npos) break; // End of the mountcfg block

            // Expect a "<key>" "<path>" pair on this line.
            size_t firstQuote = trimmed.find_first_of('"');
            if (firstQuote == string::npos) continue;
            size_t secondQuote = trimmed.find_first_of('"', firstQuote + 1);
            if (secondQuote == string::npos) continue;
            size_t thirdQuote = trimmed.find_first_of('"', secondQuote + 1);
            if (thirdQuote == string::npos) continue;
            size_t fourthQuote = trimmed.find_first_of('"', thirdQuote + 1);
            if (fourthQuote == string::npos) continue;

            string mountKey = trimmed.substr(firstQuote + 1, secondQuote - firstQuote - 1);
            string mountPath = trimmed.substr(thirdQuote + 1, fourthQuote - thirdQuote - 1);
            if (mountPath.empty()) continue;

            fs::path resolvedMountPath = fs::path(mountPath).lexically_normal();
            if (!fs::exists(resolvedMountPath) || !fs::is_directory(resolvedMountPath)) {
                cerr << "[AssetManager] GMod mount.cfg entry \"" << mountKey
                     << "\" points to a path that doesn't exist: " << mountPath << '\n';
                continue;
            }

            SearchPath sp;
            sp.path = resolvedMountPath;
            searchPaths.push_back(sp);

            cerr << "[AssetManager] GMod mount.cfg mounted \"" << mountKey << "\" -> " << mountPath << '\n';
        }
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

        // Newer Source games (Portal 2, Left 4 Dead 2, CS:GO, Alien Swarm and Alien
        // Swarm: Reactive Drop) instead pack their base content INSIDE the mod folder
        // itself as one or more "*_dir.vpk" archives (e.g. "portal2/pak01_dir.vpk"),
        // rather than as a sibling "<foldername>_dir.vpk" next to it. Without checking
        // for this too, virtually all of these games' models (which live only inside
        // those VPKs) go completely undetected even though the plain "models" folder
        // check below finds nothing on disk to fall back on.
        //
        // Rather than assuming a fixed "pakNN" naming scheme (some games use different
        // prefixes/numbering - e.g. Alien Swarm: Reactive Drop's own base content isn't
        // necessarily named "pak01"/"pak02"), scan every "*_dir.vpk" archive found
        // directly inside the mod folder itself. This also naturally covers any number
        // of incrementally-numbered pak files (pak01, pak02, pak03, ...) without having
        // to guess how many there are or what they're called.
        if (fs::exists(dirPath) && fs::is_directory(dirPath)) {
            try {
                for (const auto& entry : fs::directory_iterator(dirPath, fs::directory_options::skip_permission_denied)) {
                    if (!entry.is_regular_file()) continue;
                    const fs::path& candidate = entry.path();
                    if (candidate.extension() == ".vpk" && candidate.stem().string().ends_with("_dir")) {
                        ScanVpkForModels(candidate, out);
                    }
                }
            }
            catch (const std::exception& e) {
                cerr << "[AssetManager] Error scanning for *_dir.vpk in " << dirPath << ": " << e.what() << '\n';
            }
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

        std::lock_guard<std::mutex> vpkLock(OpenVpkMutex);
        OpenVpkCache.clear();
    }
};