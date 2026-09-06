#pragma once

// Detects a Hammer++ or stock Hammer editor installation associated with a given
// Source Engine game, and resolves the correct "sprinkle" preset folder + filename
// for whichever editor is found.
//
// Priority order (matches how the Sprinkle tool for Hammer++ actually looks for
// presets): 64-bit Hammer++ > 32-bit Hammer++ > stock Hammer.
//
// Folder layout on disk (typical Source SDK / Source 2013 style game):
//   <SteamLibrary>/steamapps/common/<Game>/          <- gamePath (e.g. "team fortress 2/tf")
//   <SteamLibrary>/steamapps/common/<Game>/../bin/            <- 32-bit tools
//   <SteamLibrary>/steamapps/common/<Game>/../bin/x64/        <- 64-bit tools (most games)
//   <SteamLibrary>/steamapps/common/<Game>/../bin/win64/      <- 64-bit tools (Garry's Mod uses this)
//
// Hammer++ ships as hammerplusplus.exe (or HammerPlusPlus.exe) inside those bin
// folders, alongside a "hammerplusplus" data folder used for its own scripts/presets.
// Stock Hammer ships as hammer.exe directly inside the game's own folder (mod-specific,
// not shared bin), e.g. "<Game>/tf/hammer.exe" or similar, and its scripts live under
// the game folder itself rather than under bin/.
#include <windows.h>
#include <string>
#include <filesystem>
#include <algorithm>
#include <cctype>

using namespace std;
namespace fs = filesystem;

enum class HammerEditorType {
	None,
	HammerPlusPlus,
	StockHammer
};

struct HammerInstallInfo {
	HammerEditorType type = HammerEditorType::None;
	fs::path editorExePath;   // Full path to the hammer(++).exe that was found
	fs::path presetFolder;    // Folder where the .txt preset should be saved
	bool is64Bit = false;     // Only meaningful when type == HammerPlusPlus
};

class HammerDetector {
public:
	// gamePath is the SourceGame::path detected by SteamDetector (e.g.
	// ".../steamapps/common/Team Fortress 2/tf" or ".../GarrysMod/garrysmod").
	// The "bin" folder used by the tools (hammer++/stock hammer/etc.) normally
	// lives one level up from the mod folder, i.e. next to it - so we look both
	// at gamePath itself and at gamePath's parent for a "bin" directory.
	static HammerInstallInfo Detect(const string& gamePath) {
		HammerInstallInfo result;

		fs::path modPath = fs::path(gamePath);
		fs::path gameRoot = modPath.parent_path(); // e.g. ".../Team Fortress 2"

		// Candidate "bin" locations: some games put tools next to the mod folder
		// (gameRoot/bin), others keep everything inside the mod folder itself
		// (modPath/bin) - check both to be safe.
		vector<fs::path> binCandidates = {
			gameRoot / "bin",
			modPath / "bin"
		};

		// 1. Look for Hammer++ first (64-bit takes priority over 32-bit).
		//    64-bit subfolder name varies by game: most use "x64", but some
		//    (e.g. Garry's Mod) use "win64" instead.
		static const vector<string> x64FolderNames = { "x64", "win64" };
		static const vector<string> hppExeNames = { "hammerplusplus.exe", "HammerPlusPlus.exe" };

		for (const auto& binDir : binCandidates) {
			if (!fs::exists(binDir)) continue;

			// 64-bit Hammer++
			for (const auto& x64Folder : x64FolderNames) {
				fs::path x64Dir = binDir / x64Folder;
				for (const auto& exeName : hppExeNames) {
					fs::path exePath = x64Dir / exeName;
					if (fs::exists(exePath)) {
						result.type = HammerEditorType::HammerPlusPlus;
						result.editorExePath = exePath;
						result.is64Bit = true;
						result.presetFolder = x64Dir / "hammerplusplus" / "scripts" / "sprinkle";
						return result;
					}
				}
			}
		}

		for (const auto& binDir : binCandidates) {
			if (!fs::exists(binDir)) continue;

			// 32-bit Hammer++ (directly inside bin/, no arch subfolder)
			for (const auto& exeName : hppExeNames) {
				fs::path exePath = binDir / exeName;
				if (fs::exists(exePath)) {
					result.type = HammerEditorType::HammerPlusPlus;
					result.editorExePath = exePath;
					result.is64Bit = false;
					result.presetFolder = binDir / "hammerplusplus" / "scripts" / "sprinkle";
					return result;
				}
			}
		}

		// 2. Fall back to stock Hammer (hammer.exe), which normally lives directly
		//    inside the shared "bin" folder as well (bin/hammer.exe), same as Hammer++
		//    without the ++ arch split - but some older mods keep it in the mod folder.
		static const vector<string> stockExeNames = { "hammer.exe" };
		vector<fs::path> stockCandidates = binCandidates;
		stockCandidates.push_back(modPath); // last resort: mod folder itself

		for (const auto& dir : stockCandidates) {
			if (!fs::exists(dir)) continue;
			for (const auto& exeName : stockExeNames) {
				fs::path exePath = dir / exeName;
				if (fs::exists(exePath)) {
					result.type = HammerEditorType::StockHammer;
					result.editorExePath = exePath;
					result.is64Bit = false;
					// Stock Hammer presets are requested to live under the *game*
					// folder (not bin): <gamePath>/scripts/hammer/sprinkle
					result.presetFolder = modPath / "scripts" / "hammer" / "sprinkle";
					return result;
				}
			}
		}

		return result; // type == None if nothing was found
	}

	// Normalizes a user-facing preset name into a safe lowercase, underscore-separated
	// filename stem (no extension), e.g. "Example: Jungle Foliage" -> "example_jungle_foliage".
	static string NormalizePresetFileName(const string& presetName) {
		string normalized;
		normalized.reserve(presetName.size());

		bool lastWasUnderscore = false;
		for (char c : presetName) {
			if (isspace(static_cast<unsigned char>(c))) {
				if (!lastWasUnderscore && !normalized.empty()) {
					normalized.push_back('_');
					lastWasUnderscore = true;
				}
				continue;
			}

			// Strip characters that are not safe/typical in filenames (keep
			// letters, digits, underscore and hyphen).
			if (isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') {
				normalized.push_back(static_cast<char>(tolower(static_cast<unsigned char>(c))));
				lastWasUnderscore = false;
			}
			// Anything else (":", "!", etc.) is simply dropped.
		}

		// Trim a trailing underscore left over from a trailing space/dropped char.
		while (!normalized.empty() && normalized.back() == '_') {
			normalized.pop_back();
		}

		return normalized.empty() ? "preset" : normalized;
	}

	// Convenience helper: given a gamePath and a preset name, resolves the full
	// destination .txt path the preset should be written to, creating the
	// destination folder if needed. Returns an empty path if no Hammer/Hammer++
	// installation could be found for this game.
	static fs::path ResolvePresetSavePath(const string& gamePath, const string& presetName) {
		HammerInstallInfo info = Detect(gamePath);
		if (info.type == HammerEditorType::None) {
			return {};
		}

		std::error_code ec;
		fs::create_directories(info.presetFolder, ec);

		string fileName = NormalizePresetFileName(presetName) + ".txt";
		return info.presetFolder / fileName;
	}
};
