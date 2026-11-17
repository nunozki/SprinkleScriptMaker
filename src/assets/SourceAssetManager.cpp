#include "assets/SourceAssetManager.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

#include <vpkpp/vpkpp.h>

#include "util/StringUtils.h"

namespace fs = std::filesystem;

namespace SourceAssetManager {
	std::atomic<std::uint64_t> IndexGeneration{ 0 };
}

namespace {

	using ModelIndex = std::map<std::string, std::string>; // model path -> VPK archive or folder

	struct SearchPath {
		fs::path path;
		bool isVpk = false;
		bool isWildcard = false;
	};

	// ---------------------------------------------------------------------
	// Shared state
	// ---------------------------------------------------------------------

	// Guards ModelLocations, GameSearchPaths and SkinCountCache. IndexGameModels() runs on a
	// background thread while the UI thread reads these (model list, skin counts, preview).
	std::mutex IndexMutex;

	// Which VPK or folder each indexed model belongs to.
	ModelIndex ModelLocations;

	// The search paths of the last indexed game. Reused for material/texture lookups,
	// which aren't part of the model index.
	std::vector<SearchPath> GameSearchPaths;

	// Skin count per model path, so per-frame UI lookups don't re-read the same file.
	std::map<std::string, int> SkinCountCache;

	// Already-opened VPK archives, keyed by resolved archive path. Opening a VPK parses its
	// whole directory index, which is expensive; without this cache, scrolling the model
	// list would reopen the same large archive once per model.
	std::map<std::string, std::shared_ptr<vpkpp::PackFile>> OpenVpkCache;
	std::mutex OpenVpkMutex;

	// Offset of the "numskinfamilies" field in a studio model (.mdl) header.
	constexpr std::streamoff kMdlSkinFamiliesOffset = 224;

	// ---------------------------------------------------------------------
	// Small helpers
	// ---------------------------------------------------------------------

	long long ElapsedMs(std::chrono::steady_clock::time_point since) {
		return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count();
	}

	std::vector<std::string> KeysOf(const ModelIndex& index) {
		std::vector<std::string> keys;
		keys.reserve(index.size());
		for (const auto& [mdl, location] : index) {
			keys.push_back(mdl);
		}
		return keys;
	}

	// Multi-chunk VPKs are split into "<name>_dir.vpk" (the directory index) plus
	// "<name>_000.vpk", "<name>_001.vpk", ... (raw data with no index of their own).
	// Only the "_dir.vpk" file is ever worth opening.
	bool IsDirVpk(const fs::path& path) {
		return path.extension() == ".vpk" && path.stem().string().ends_with("_dir");
	}

	// gameinfo.txt often references a VPK by its logical name (e.g. "garrysmod.vpk"), while
	// on disk the archive is "<name>_dir.vpk". The engine resolves this when mounting, so we
	// do the same.
	fs::path ResolveActualVpkPath(const fs::path& vpkPath) {
		if (fs::exists(vpkPath)) return vpkPath;

		fs::path dirVariant = vpkPath.parent_path() / (vpkPath.stem().string() + "_dir" + vpkPath.extension().string());
		if (fs::exists(dirVariant)) return dirVariant;

		return vpkPath; // Not found either way; let the caller fail gracefully
	}

	// Returns a shared handle to a VPK archive, opening and parsing it only once.
	std::shared_ptr<vpkpp::PackFile> GetOrOpenVpk(const std::string& actualVpkPath) {
		{
			std::lock_guard<std::mutex> lock(OpenVpkMutex);
			auto it = OpenVpkCache.find(actualVpkPath);
			if (it != OpenVpkCache.end()) return it->second;
		}

		auto vpk = vpkpp::VPK::open(actualVpkPath);
		std::shared_ptr<vpkpp::PackFile> shared = vpk ? std::shared_ptr<vpkpp::PackFile>(std::move(vpk)) : nullptr;

		std::lock_guard<std::mutex> lock(OpenVpkMutex);
		OpenVpkCache[actualVpkPath] = shared;
		return shared;
	}

	// Reads relPath out of a (cached) VPK archive. Returns false if the archive can't be
	// opened or doesn't contain the entry.
	bool TryReadFromVpk(const fs::path& vpkPath, const std::string& relPath, std::vector<std::byte>& out) {
		auto vpk = GetOrOpenVpk(ResolveActualVpkPath(vpkPath).string());
		if (!vpk) return false;

		auto data = vpk->readEntry(relPath);
		if (!data.has_value()) return false;

		out = std::move(*data);
		return true;
	}

	bool ReadFileBytes(const fs::path& path, std::vector<std::byte>& out) {
		if (!fs::exists(path)) return false;
		std::ifstream stream(path, std::ios::binary | std::ios::ate);
		if (!stream.is_open()) return false;

		std::streamsize size = stream.tellg();
		if (size <= 0) return false;
		stream.seekg(0, std::ios::beg);

		out.resize(static_cast<std::size_t>(size));
		return static_cast<bool>(stream.read(reinterpret_cast<char*>(out.data()), size));
	}

	// ---------------------------------------------------------------------
	// Scanning
	// ---------------------------------------------------------------------

	void ScanVpkForModels(const fs::path& vpkPath, ModelIndex& out) {
		auto startTime = std::chrono::steady_clock::now();
		fs::path actualVpkPath = ResolveActualVpkPath(vpkPath);
		if (!fs::exists(actualVpkPath)) return;

		// Valve splits shared content into VPKs by asset type (e.g. "hl2_textures.vpk",
		// "hl2_sound_vo_english.vpk"). Texture and sound archives never contain models, and
		// parsing a VPK's directory index is the expensive part, so skip them entirely.
		std::string lowerName = actualVpkPath.filename().string();
		std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (lowerName.find("_textures") != std::string::npos || lowerName.find("_sound") != std::string::npos) {
			return;
		}

		try {
			auto openStart = std::chrono::steady_clock::now();
			auto vpk = vpkpp::VPK::open(actualVpkPath.string());
			auto openElapsedMs = ElapsedMs(openStart);
			if (!vpk) return;

			const auto& baked = vpk->getBakedEntries();
			for (auto it = baked.begin(); it != baked.end(); ++it) {
				// In tsl::htrie_map the entry path is stored as the map key
				std::string fullEntryPath = std::string(it.key());

				if (fs::path(fullEntryPath).extension() == ".mdl" && fullEntryPath.rfind("models/", 0) == 0) {
					out[fullEntryPath] = actualVpkPath.string();
				}
			}

			std::cerr << "[AssetManager][Timing] VPK " << actualVpkPath.filename().string() << ": open=" << openElapsedMs << "ms, total=" << ElapsedMs(startTime) << "ms\n";
		}
		catch (const std::exception& e) {
			std::cerr << "[AssetManager] Error reading VPK " << actualVpkPath << ": " << e.what() << '\n';
		}
		catch (...) {
			std::cerr << "[AssetManager] Unknown error reading VPK " << actualVpkPath << "!\n";
		}
	}

	void ScanDirectoryForModels(const fs::path& dirPath, ModelIndex& out) {
		auto startTime = std::chrono::steady_clock::now();

		// Plain-folder search paths (e.g. "garrysmod") are also implicitly backed by a sibling
		// "<foldername>_dir.vpk" when present, even though gameinfo.txt never mentions it.
		// Garry's Mod ships most of its base content this way.
		fs::path vpkCompanion = dirPath.parent_path() / (dirPath.filename().string() + "_dir.vpk");
		if (fs::exists(vpkCompanion)) {
			ScanVpkForModels(vpkCompanion, out);
		}

		if (!fs::exists(dirPath) || !fs::is_directory(dirPath)) return;

		// Newer games (Portal 2, L4D2, CS:GO, Alien Swarm, ...) pack their base content
		// inside the mod folder itself as one or more "*_dir.vpk" archives. The names vary
		// between games (not always "pakNN"), so scan every "*_dir.vpk" found there.
		try {
			for (const auto& entry : fs::directory_iterator(dirPath, fs::directory_options::skip_permission_denied)) {
				if (entry.is_regular_file() && IsDirVpk(entry.path())) {
					ScanVpkForModels(entry.path(), out);
				}
			}
		}
		catch (const std::exception& e) {
			std::cerr << "[AssetManager] Error scanning for *_dir.vpk in " << dirPath << ": " << e.what() << '\n';
		}

		// Mod folders always keep .mdl files under "models/". Walking only that subfolder
		// skips huge irrelevant trees (materials/, sound/, maps/, ...).
		fs::path modelsDir = dirPath / "models";
		if (!fs::exists(modelsDir) || !fs::is_directory(modelsDir)) return;

		try {
			for (const auto& entry : fs::recursive_directory_iterator(modelsDir, fs::directory_options::skip_permission_denied)) {
				if (entry.is_regular_file() && entry.path().extension() == ".mdl") {
					std::string relativePath = "models/" + fs::relative(entry.path(), modelsDir).generic_string();
					out[relativePath] = dirPath.string();
				}
			}
		}
		catch (const fs::filesystem_error& e) {
			// e.g. paths that are too long, or locked files
			std::cerr << "[AssetManager] Filesystem error scanning " << modelsDir << ": " << e.what() << '\n';
		}
		catch (const std::exception& e) {
			std::cerr << "[AssetManager] Error scanning " << modelsDir << ": " << e.what() << '\n';
		}
		catch (...) {
			std::cerr << "[AssetManager] Unknown error scanning " << modelsDir << "!\n";
		}

		auto elapsedMs = ElapsedMs(startTime);
		if (elapsedMs > 5) {
			std::cerr << "[AssetManager][Timing] Directory " << dirPath << ": " << elapsedMs << "ms\n";
		}
	}

	// ---------------------------------------------------------------------
	// Search path discovery
	// ---------------------------------------------------------------------

	std::vector<SearchPath> ParseGameInfo(const fs::path& gameinfoPath, const fs::path& engineRoot, const fs::path& gameinfoFolder) {
		std::vector<SearchPath> resolvedPaths;
		std::ifstream file(gameinfoPath);
		if (!file.is_open()) return resolvedPaths;

		std::string line;
		bool insideFileSystem = false, insideSearchPaths = false;

		while (std::getline(file, line)) {
			std::string cleanLine(StringUtils::StripLineComment(line));

			if (cleanLine.find("FileSystem") != std::string::npos) insideFileSystem = true;
			if (insideFileSystem && cleanLine.find("SearchPaths") != std::string::npos) insideSearchPaths = true;
			if (!insideSearchPaths) continue;

			if (cleanLine.find('}') != std::string::npos && cleanLine.find('{') == std::string::npos) break;

			// Entries look like either `game "path"` or `game path`.
			std::size_t firstQuote = cleanLine.find_first_of('"');
			std::size_t lastQuote = cleanLine.find_last_of('"');
			std::string rawPath;

			if (firstQuote != std::string::npos && lastQuote != std::string::npos && firstQuote != lastQuote) {
				rawPath = cleanLine.substr(firstQuote + 1, lastQuote - firstQuote - 1);
			}
			else {
				std::istringstream iss(cleanLine);
				std::string key, val;
				if (iss >> key >> val) rawPath = val;
			}

			if (rawPath.empty()) continue;

			std::string resolved = rawPath;
			constexpr std::string_view kGameInfoMacro = "|gameinfo_path|";
			constexpr std::string_view kEngineMacro = "|all_source_engine_paths|";

			std::size_t macroPos = resolved.find(kGameInfoMacro);
			if (macroPos != std::string::npos) resolved.replace(macroPos, kGameInfoMacro.size(), gameinfoFolder.string() + "/");

			macroPos = resolved.find(kEngineMacro);
			if (macroPos != std::string::npos) resolved.replace(macroPos, kEngineMacro.size(), engineRoot.string() + "/");

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
		return resolvedPaths;
	}

	// Garry's Mod mounts every folder in "garrysmod/addons/" at runtime (legacy/local
	// addons; Workshop addons live elsewhere). They're never listed in gameinfo.txt, so add
	// each addon folder that contains "models" or "materials" as an extra search path.
	void AppendGModAddonSearchPaths(const fs::path& gameRootPath, std::vector<SearchPath>& searchPaths) {
		fs::path addonsDir = gameRootPath / "garrysmod" / "addons";
		if (!fs::exists(addonsDir) || !fs::is_directory(addonsDir)) return;

		try {
			for (const auto& entry : fs::directory_iterator(addonsDir, fs::directory_options::skip_permission_denied)) {
				if (!entry.is_directory()) continue;

				const fs::path& addonPath = entry.path();
				bool hasModels = fs::is_directory(addonPath / "models");
				bool hasMaterials = fs::is_directory(addonPath / "materials");

				if (hasModels || hasMaterials) {
					SearchPath sp;
					sp.path = addonPath.lexically_normal();
					searchPaths.push_back(sp);
				}
			}
		}
		catch (const std::exception& e) {
			std::cerr << "[AssetManager] Error scanning GMod addons folder " << addonsDir << ": " << e.what() << '\n';
		}
	}

	// Garry's Mod can mount other games' content through "garrysmod/cfg/mount.cfg":
	//
	//   "mountcfg"
	//   {
	//       "cstrike"   "C:\...\Counter-Strike Source Dedicated Server\cstrike"
	//       "tf"        "C:\mytf2server\tf"
	//   }
	//
	// Every uncommented "key" "path" pair is an extra folder to scan. Lines starting with
	// "//" are ignored, including the commented-out examples Valve ships in the block.
	void AppendGModMountCfgSearchPaths(const fs::path& gameRootPath, std::vector<SearchPath>& searchPaths) {
		std::ifstream file(gameRootPath / "garrysmod" / "cfg" / "mount.cfg");
		if (!file.is_open()) return;

		std::string line;
		bool insideBlock = false;

		while (std::getline(file, line)) {
			std::string_view trimmed = StringUtils::Trim(line);
			if (trimmed.empty()) continue;
			if (trimmed.rfind("//", 0) == 0) continue;

			if (!insideBlock) {
				if (trimmed.find('{') != std::string_view::npos) insideBlock = true;
				continue;
			}

			if (trimmed.find('}') != std::string_view::npos) break; // End of the mountcfg block

			std::string mountKey = StringUtils::ExtractQuoted(trimmed, 0);
			std::string mountPath = StringUtils::ExtractQuoted(trimmed, 1);
			if (mountPath.empty()) continue;

			fs::path resolvedMountPath = fs::path(mountPath).lexically_normal();
			if (!fs::is_directory(resolvedMountPath)) {
				std::cerr << "[AssetManager] GMod mount.cfg entry \"" << mountKey
				          << "\" points to a path that doesn't exist: " << mountPath << '\n';
				continue;
			}

			SearchPath sp;
			sp.path = resolvedMountPath;
			searchPaths.push_back(sp);

			std::cerr << "[AssetManager] GMod mount.cfg mounted \"" << mountKey << "\" -> " << mountPath << '\n';
		}
	}

	// ---------------------------------------------------------------------
	// Disk cache
	// ---------------------------------------------------------------------

	// A stable per-game cache file under %TEMP%, named after a hash of the game's root path.
	fs::path GetIndexCachePath(const std::string& gameRootPath) {
		std::size_t h = std::hash<std::string>{}(gameRootPath);
		fs::path cacheDir = fs::temp_directory_path() / "SprinkleScriptMaker" / "AssetIndexCache";
		std::error_code ec;
		fs::create_directories(cacheDir, ec);
		std::ostringstream name;
		name << std::hex << h << ".idx";
		return cacheDir / name.str();
	}

	// Cheap fingerprint of everything that could invalidate a cached index: the last-write
	// times of gameinfo.txt and of every search path (VPKs and folders alike). Any mod
	// update, VPK repack or new addon touches at least one of them.
	std::uint64_t ComputeSearchPathsFingerprint(const fs::path& gameinfoFile, const std::vector<SearchPath>& searchPaths, const fs::path& gameRootPath) {
		std::uint64_t fp = 1469598103934665603ULL; // FNV offset basis
		auto mix = [&fp](std::uint64_t value) {
			fp ^= value;
			fp *= 1099511628211ULL; // FNV prime
		};

		std::error_code ec;
		auto mixTime = [&](const fs::path& p) {
			auto t = fs::last_write_time(p, ec);
			if (!ec) {
				mix(static_cast<std::uint64_t>(t.time_since_epoch().count()));
			}
		};

		mixTime(gameinfoFile);
		// GMod's mount.cfg and addons folder, so editing mount.cfg or adding/removing an
		// addon invalidates the cache too.
		mixTime(gameRootPath / "garrysmod" / "cfg" / "mount.cfg");
		mixTime(gameRootPath / "garrysmod" / "addons");

		for (const auto& sp : searchPaths) {
			mixTime(sp.path);
			mixTime(sp.path.parent_path() / (sp.path.filename().string() + "_dir.vpk"));

			// Every "*_dir.vpk" directly inside the mod folder (pak01_dir.vpk, ...)
			if (fs::is_directory(sp.path)) {
				std::error_code dirEc;
				for (const auto& entry : fs::directory_iterator(sp.path, fs::directory_options::skip_permission_denied, dirEc)) {
					if (entry.is_regular_file() && IsDirVpk(entry.path())) {
						mixTime(entry.path());
					}
				}
			}
		}
		return fp;
	}

	// Cache format: the fingerprint on the first line, then "<modelPath>\t<sourcePath>" lines.
	// The cache is rejected outright on any mismatch or error.
	bool LoadIndexCache(const fs::path& cacheFile, std::uint64_t expectedFingerprint, ModelIndex& out) {
		std::ifstream in(cacheFile);
		if (!in.is_open()) return false;

		std::string headerLine;
		if (!std::getline(in, headerLine)) return false;

		std::uint64_t storedFingerprint = 0;
		try {
			storedFingerprint = std::stoull(headerLine);
		}
		catch (...) {
			return false;
		}
		if (storedFingerprint != expectedFingerprint) return false;

		ModelIndex loaded;
		std::string line;
		while (std::getline(in, line)) {
			std::size_t tab = line.find('\t');
			if (tab == std::string::npos) continue;
			loaded[line.substr(0, tab)] = line.substr(tab + 1);
		}

		out = std::move(loaded);
		return true;
	}

	void SaveIndexCache(const fs::path& cacheFile, std::uint64_t fingerprint, const ModelIndex& data) {
		std::ofstream out(cacheFile, std::ios::trunc);
		if (!out.is_open()) return;

		out << fingerprint << '\n';
		for (const auto& [mdl, location] : data) {
			out << mdl << '\t' << location << '\n';
		}
	}

	// Publishes a freshly built index as the shared ModelLocations, dropping the caches that
	// belonged to whatever was indexed before.
	void PublishIndex(ModelIndex& localModelLocations) {
		std::lock_guard<std::mutex> lock(IndexMutex);
		ModelLocations = std::move(localModelLocations);
		SkinCountCache.clear();

		std::lock_guard<std::mutex> vpkLock(OpenVpkMutex);
		OpenVpkCache.clear();
	}

	// Looks up which VPK or folder an indexed model lives in. Returns false if it isn't indexed.
	bool FindModelLocation(const std::string& modelPath, std::string& outLocation) {
		std::lock_guard<std::mutex> lock(IndexMutex);
		auto it = ModelLocations.find(modelPath);
		if (it == ModelLocations.end()) return false;
		outLocation = it->second;
		return true;
	}

	// Builds one independent scan task per VPK archive / folder reachable from the search paths.
	std::vector<std::function<ModelIndex()>> BuildScanTasks(const std::vector<SearchPath>& searchPaths) {
		std::vector<std::function<ModelIndex()>> tasks;

		auto addDirTask = [&tasks](fs::path dir) {
			tasks.push_back([dir]() {
				ModelIndex local;
				ScanDirectoryForModels(dir, local);
				return local;
			});
		};
		auto addVpkTask = [&tasks](fs::path vpk) {
			tasks.push_back([vpk]() {
				ModelIndex local;
				ScanVpkForModels(vpk, local);
				return local;
			});
		};

		for (const auto& sp : searchPaths) {
			if (sp.isWildcard) {
				if (!fs::is_directory(sp.path)) continue;
				try {
					for (const auto& subEntry : fs::directory_iterator(sp.path, fs::directory_options::skip_permission_denied)) {
						if (subEntry.is_directory()) addDirTask(subEntry.path());
						else if (IsDirVpk(subEntry.path())) addVpkTask(subEntry.path());
					}
				}
				catch (const std::exception& e) {
					std::cerr << "[AssetManager] Error scanning wildcard path " << sp.path << ": " << e.what() << '\n';
				}
			}
			else if (sp.isVpk) {
				addVpkTask(sp.path);
			}
			else {
				addDirTask(sp.path);
			}
		}
		return tasks;
	}

	// Finds the first folder directly inside gameRootPath that contains a gameinfo.txt.
	// Throws on filesystem errors.
	bool FindGameInfo(const fs::path& gameRootPath, const std::function<bool()>& isStale, fs::path& outFolder, fs::path& outFile) {
		for (const auto& entry : fs::directory_iterator(gameRootPath, fs::directory_options::skip_permission_denied)) {
			if (isStale()) return false;
			if (!entry.is_directory()) continue;

			fs::path target = entry.path() / "gameinfo.txt";
			if (fs::exists(target)) {
				outFolder = entry.path();
				outFile = target;
				return true;
			}
		}
		return false;
	}

} // namespace

namespace SourceAssetManager {

	std::vector<std::string> IndexGameModels(const std::string& gameRootPath, std::uint64_t myGeneration) {
		ModelIndex localModelLocations; // Built locally, published at the end

		auto isStale = [myGeneration]() { return IndexGeneration.load() != myGeneration; };

		fs::path gameinfoFolder;
		fs::path gameinfoFile;

		try {
			FindGameInfo(gameRootPath, isStale, gameinfoFolder, gameinfoFile);
		}
		catch (const std::exception& e) {
			std::cerr << "[AssetManager] Error locating gameinfo.txt in " << gameRootPath << ": " << e.what() << '\n';
			if (!isStale()) PublishIndex(localModelLocations);
			return {};
		}

		if (isStale()) return {};

		if (gameinfoFile.empty()) {
			PublishIndex(localModelLocations);
			return {};
		}

		auto indexStartTime = std::chrono::steady_clock::now();

		std::vector<SearchPath> searchPaths = ParseGameInfo(gameinfoFile, gameRootPath, gameinfoFolder);

		// Garry's Mod mounts addons and mount.cfg content at runtime; neither is in gameinfo.txt.
		AppendGModAddonSearchPaths(gameRootPath, searchPaths);
		AppendGModMountCfgSearchPaths(gameRootPath, searchPaths);

		if (isStale()) return {};

		{
			std::lock_guard<std::mutex> lock(IndexMutex);
			GameSearchPaths = searchPaths;
		}

		// Re-scanning every VPK and folder is by far the slowest part of indexing, and the
		// result only changes when the game's files do. Try the disk cache first.
		fs::path cacheFile = GetIndexCachePath(gameRootPath);
		std::uint64_t fingerprint = ComputeSearchPathsFingerprint(gameinfoFile, searchPaths, gameRootPath);

		if (LoadIndexCache(cacheFile, fingerprint, localModelLocations)) {
			if (isStale()) return {};
			std::vector<std::string> cachedKeys = KeysOf(localModelLocations);
			std::cerr << "[AssetManager][Timing] Loaded from disk cache in " << ElapsedMs(indexStartTime) << " ms (" << localModelLocations.size() << " models)\n";
			PublishIndex(localModelLocations);
			return cachedKeys;
		}

		// Each VPK or folder is independent of the others, so scan them on a small thread
		// pool (bounded by hardware concurrency) instead of one after another.
		auto tasks = BuildScanTasks(searchPaths);

		unsigned int hwThreads = std::thread::hardware_concurrency();
		std::size_t poolSize = std::max<std::size_t>(1, std::min<std::size_t>(hwThreads == 0 ? 4u : hwThreads, tasks.size()));

		std::mutex taskMutex;
		std::size_t nextTask = 0;
		std::mutex mergeMutex;

		auto worker = [&]() {
			while (true) {
				if (isStale()) return; // The selected game changed again; abandon this run
				std::size_t myIndex;
				{
					std::lock_guard<std::mutex> lock(taskMutex);
					if (nextTask >= tasks.size()) return;
					myIndex = nextTask++;
				}
				try {
					ModelIndex result = tasks[myIndex]();
					std::lock_guard<std::mutex> lock(mergeMutex);
					for (auto& [mdl, location] : result) {
						localModelLocations[mdl] = std::move(location);
					}
				}
				catch (const std::exception& e) {
					std::cerr << "[AssetManager] Error running scan task: " << e.what() << '\n';
				}
				catch (...) {
					std::cerr << "[AssetManager] Unknown error running scan task!\n";
				}
			}
		};

		std::cerr << "[AssetManager][Timing] Running " << tasks.size() << " scan tasks across " << poolSize << " threads...\n";

		std::vector<std::thread> workers;
		workers.reserve(poolSize);
		for (std::size_t i = 0; i < poolSize; ++i) {
			workers.emplace_back(worker);
		}
		for (auto& w : workers) w.join();

		// A newer game was selected mid-scan: discard the (possibly incomplete) results so they
		// neither pollute the disk cache nor overwrite the newer run's state.
		if (isStale()) return {};

		std::cerr << "[AssetManager][Timing] All scan tasks finished in " << ElapsedMs(indexStartTime) << " ms\n";

		std::vector<std::string> keys = KeysOf(localModelLocations);
		SaveIndexCache(cacheFile, fingerprint, localModelLocations);
		PublishIndex(localModelLocations);
		return keys;
	}

	std::map<std::string, std::string> GetModelLocationsSnapshot() {
		std::lock_guard<std::mutex> lock(IndexMutex);
		return ModelLocations;
	}

	int GetSkinCount(const std::string& modelPath) {
		std::string sourcePath;
		{
			std::lock_guard<std::mutex> lock(IndexMutex);
			auto cacheIt = SkinCountCache.find(modelPath);
			if (cacheIt != SkinCountCache.end()) return cacheIt->second;

			auto it = ModelLocations.find(modelPath);
			if (it == ModelLocations.end()) return -1; // Not indexed
			sourcePath = it->second;
		}

		int skinFamilies = 0;
		bool readOk = false;

		if (sourcePath.ends_with(".vpk")) {
			try {
				auto vpk = GetOrOpenVpk(ResolveActualVpkPath(sourcePath).string());
				if (!vpk) return -1;

				auto data = vpk->readEntry(modelPath);
				if (data.has_value() && data->size() >= kMdlSkinFamiliesOffset + sizeof(skinFamilies)) {
					std::memcpy(&skinFamilies, data->data() + kMdlSkinFamiliesOffset, sizeof(skinFamilies));
					readOk = true;
				}
			}
			catch (const std::exception& e) {
				std::cerr << "[AssetManager] Error reading skin count from VPK " << sourcePath << ": " << e.what() << '\n';
				return -1;
			}
			catch (...) {
				std::cerr << "[AssetManager] Unknown error reading skin count from VPK " << sourcePath << "!\n";
				return -1;
			}
		}
		else {
			std::ifstream stream(fs::path(sourcePath) / modelPath, std::ios::binary);
			if (stream.is_open()) {
				stream.seekg(kMdlSkinFamiliesOffset);
				readOk = static_cast<bool>(stream.read(reinterpret_cast<char*>(&skinFamilies), sizeof(skinFamilies)));
			}
		}

		if (!readOk) return -1;

		std::lock_guard<std::mutex> lock(IndexMutex);
		SkinCountCache[modelPath] = skinFamilies;
		return skinFamilies;
	}

	bool LoadStudioModelBytes(const std::string& modelPath,
	                          std::vector<std::byte>& mdlOut,
	                          std::vector<std::byte>& vtxOut,
	                          std::vector<std::byte>& vvdOut,
	                          std::string& errorOut) {
		std::string sourcePath;
		if (!FindModelLocation(modelPath, sourcePath)) {
			errorOut = "Model not found in index: " + modelPath;
			return false;
		}
		if (!modelPath.ends_with(".mdl")) {
			errorOut = "Not a .mdl path: " + modelPath;
			return false;
		}

		const std::string base = modelPath.substr(0, modelPath.size() - 4); // strip ".mdl"
		const std::string vvdPath = base + ".vvd";
		// VTX files can be compiled for different renderers; try the common ones in order.
		const std::vector<std::string> vtxCandidates = { base + ".dx90.vtx", base + ".dx80.vtx", base + ".sw.vtx" };

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

			bool foundVtx = std::any_of(vtxCandidates.begin(), vtxCandidates.end(),
				[&](const std::string& candidate) { return ReadFileBytes(root / candidate, vtxOut); });
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

	bool LoadAssetBytes(const std::string& relPath, std::vector<std::byte>& out, std::string& errorOut) {
		std::vector<SearchPath> searchPaths;
		{
			std::lock_guard<std::mutex> lock(IndexMutex);
			searchPaths = GameSearchPaths;
		}

		auto found = [&]() {
			errorOut.clear();
			return true;
		};

		for (const auto& sp : searchPaths) {
			if (sp.isWildcard) {
				if (!fs::is_directory(sp.path)) continue;
				try {
					for (const auto& subEntry : fs::directory_iterator(sp.path, fs::directory_options::skip_permission_denied)) {
						const fs::path& subPath = subEntry.path();
						if (subEntry.is_directory()) {
							if (ReadFileBytes(subPath / relPath, out)) return found();
						}
						else if (IsDirVpk(subPath)) {
							if (TryReadFromVpk(subPath, relPath, out)) return found();
						}
					}
				}
				catch (const std::exception&) {
					// Ignore and keep searching the other search paths
				}
			}
			else if (sp.isVpk) {
				if (TryReadFromVpk(sp.path, relPath, out)) return found();
			}
			else {
				if (ReadFileBytes(sp.path / relPath, out)) return found();

				// Same implicit sibling "<foldername>_dir.vpk" as ScanDirectoryForModels(): models
				// indexed from it need their materials looked up there too.
				fs::path vpkCompanion = sp.path.parent_path() / (sp.path.filename().string() + "_dir.vpk");
				if (fs::exists(vpkCompanion) && TryReadFromVpk(vpkCompanion, relPath, out)) return found();

				// "*_dir.vpk" archives inside the mod folder itself (Portal 2, L4D2, CS:GO, ...)
				if (fs::is_directory(sp.path)) {
					try {
						for (const auto& subEntry : fs::directory_iterator(sp.path, fs::directory_options::skip_permission_denied)) {
							if (subEntry.is_regular_file() && IsDirVpk(subEntry.path())
								&& TryReadFromVpk(subEntry.path(), relPath, out)) {
								return found();
							}
						}
					}
					catch (const std::exception&) {
						// Ignore and keep searching the other search paths
					}
				}
			}
		}

		errorOut = "Asset not found in any search path: " + relPath;
		return false;
	}

} // namespace SourceAssetManager
