#include "platform/SteamDetector.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "util/StringUtils.h"

namespace fs = std::filesystem;

namespace {

	// The Source Engine games we look for.
	//
	// AppID 730 was upgraded in place by Valve to Counter-Strike 2 (Source 2) for most
	// players, but some installs (e.g. via depot downgrade tools, or backups that never got
	// the CS2 update) can still be legacy Source 1 CS:GO. Both share the same AppID and
	// manifest, so it is only accepted as CS:GO if "csgo.exe" still exists in its folder
	// (CS2 no longer ships that binary).
	//
	// AppID 4465480 is a second, separate CS:GO install (e.g. a depot-downgraded copy kept
	// side by side with the regular 730 install). When both are installed, their display
	// names get tagged so the user can tell which is which.
	const std::vector<SourceGame> kTargetGames = {
		{ "Team Fortress 2", 440, "" },
		{ "Counter-Strike: Source", 240, "" },
		{ "Garry's Mod", 4000, "" },
		{ "Alien Swarm", 630, "" },
		{ "Alien Swarm: Reactive Drop", 563560, "" },
		{ "Source SDK Base 2013 Singleplayer", 243730, "" },
		{ "Source SDK Base 2013 Multiplayer", 243750, "" },
		{ "Day of Defeat: Source", 300, "" },
		{ "Left 4 Dead 2", 550, "" },
		{ "Portal 2", 620, "" },
		{ "Half-Life 2", 220, "" },
		{ "Half-Life 2: Episode One", 380, "" },
		{ "Half-Life 2: Episode Two", 420, "" },
		{ "Counter-Strike: Global Offensive", 730, "" },
		{ "Counter-Strike: Global Offensive", 4465480, "" },
	};

	constexpr int kCsgoAppId = 730;
	constexpr int kCsgoSecondAppId = 4465480;

	// Reads the value of the first line containing `quotedKey` (e.g. "\"installdir\"")
	// in a KeyValues-style file such as an .acf manifest.
	std::string ReadFirstValue(const fs::path& file, std::string_view quotedKey) {
		std::ifstream stream(file);
		if (!stream.is_open()) return "";

		std::string line;
		while (std::getline(stream, line)) {
			if (line.find(quotedKey) != std::string::npos) {
				return StringUtils::ExtractQuoted(line, 1);
			}
		}
		return "";
	}

	// Reads the "installdir" field of an appmanifest_<AppID>.acf file. This is the actual
	// folder name under steamapps/common, which can differ from the game's display name
	// (e.g. Garry's Mod -> "GarrysMod").
	std::string GetInstallDirFromManifest(const fs::path& manifestPath) {
		return ReadFirstValue(manifestPath, "\"installdir\"");
	}

	// Reads the Steam install folder from the Windows Registry.
	std::string GetSteamPathFromRegistry() {
		HKEY hKey;
		if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
			return "";
		}

		char path[MAX_PATH];
		DWORD pathLen = sizeof(path);
		LSTATUS status = RegQueryValueExA(hKey, "SteamPath", nullptr, nullptr, reinterpret_cast<LPBYTE>(path), &pathLen);
		RegCloseKey(hKey);
		if (status != ERROR_SUCCESS) return "";

		// Steam stores the path with forward slashes
		std::string result(path);
		for (char& c : result) {
			if (c == '/') c = '\\';
		}
		return result;
	}

	// Parses libraryfolders.vdf to find extra Steam library folders (other drives, etc.).
	std::vector<std::string> GetLibraryFolders(const std::string& steamPath) {
		std::vector<std::string> folders;
		std::ifstream file(fs::path(steamPath) / "steamapps" / "libraryfolders.vdf");
		if (!file.is_open()) return folders;

		std::string line;
		while (std::getline(file, line)) {
			if (line.find("\"path\"") == std::string::npos) continue;

			std::string path = StringUtils::ExtractQuoted(line, 1);
			if (path.empty()) continue;

			// VDF files escape backslashes (e.g. "C:\\SteamLibrary")
			std::size_t pos = 0;
			while ((pos = path.find("\\\\", pos)) != std::string::npos) {
				path.replace(pos, 2, "\\");
				pos += 1;
			}
			folders.push_back(path);
		}
		return folders;
	}

} // namespace

namespace SteamDetector {

	std::vector<SourceGame> GetInstalledGames() {
		std::vector<SourceGame> installedGames;

		std::string steamPath = GetSteamPathFromRegistry();
		if (steamPath.empty()) return installedGames;

		std::vector<std::string> libraries = GetLibraryFolders(steamPath);
		libraries.push_back(steamPath); // The main Steam folder is a library too

		for (const auto& lib : libraries) {
			for (const auto& game : kTargetGames) {
				// Steam marks an installed game with steamapps/appmanifest_<AppID>.acf
				fs::path manifestPath = fs::path(lib) / "steamapps" / ("appmanifest_" + std::to_string(game.appId) + ".acf");
				if (!fs::exists(manifestPath)) continue;

				SourceGame foundGame = game;

				std::string installDir = GetInstallDirFromManifest(manifestPath);
				if (installDir.empty()) installDir = game.name; // Fallback if parsing fails

				foundGame.path = (fs::path(lib) / "steamapps" / "common" / installDir).string();

				// AppID 730 is shared between legacy CS:GO and CS2; skip it if it's CS2.
				if (game.appId == kCsgoAppId && !fs::exists(fs::path(foundGame.path) / "csgo.exe")) {
					continue;
				}

				bool alreadyAdded = std::any_of(installedGames.begin(), installedGames.end(),
					[&](const SourceGame& g) { return g.appId == game.appId; });
				if (!alreadyAdded) {
					installedGames.push_back(foundGame);
				}
			}
		}

		// If both CS:GO installs are present, tag each so they can be told apart.
		bool hasFirstCsgo = false, hasSecondCsgo = false;
		for (const auto& g : installedGames) {
			if (g.appId == kCsgoAppId) hasFirstCsgo = true;
			if (g.appId == kCsgoSecondAppId) hasSecondCsgo = true;
		}
		if (hasFirstCsgo && hasSecondCsgo) {
			for (auto& g : installedGames) {
				if (g.appId == kCsgoAppId) g.name += " (Original - AppID 730)";
				else if (g.appId == kCsgoSecondAppId) g.name += " (New - AppID 4465480)";
			}
		}

		return installedGames;
	}

} // namespace SteamDetector
