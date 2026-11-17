#pragma once

#include <string>
#include <vector>

// A Source Engine game found in one of the user's Steam libraries.
struct SourceGame {
	std::string name;
	int appId = 0;
	std::string path; // Install folder, e.g. ".../steamapps/common/Team Fortress 2"
};

namespace SteamDetector {

	// Finds the supported Source Engine games installed across every Steam library
	// (the main Steam folder plus all extra libraries listed in libraryfolders.vdf).
	// Returns an empty list if Steam isn't installed.
	std::vector<SourceGame> GetInstalledGames();

} // namespace SteamDetector
