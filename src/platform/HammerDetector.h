#pragma once

// Detects a Hammer++ or stock Hammer installation for a Source Engine game and resolves
// the "sprinkle" preset folder and file name for whichever editor is found.
//
// Priority order (matches where the Sprinkle tool looks for presets):
// 64-bit Hammer++ > 32-bit Hammer++ > stock Hammer.
//
// Typical layout on disk:
//   <SteamLibrary>/steamapps/common/<Game>/              <- gamePath
//   <SteamLibrary>/steamapps/common/<Game>/bin/          <- 32-bit tools
//   <SteamLibrary>/steamapps/common/<Game>/bin/x64/      <- 64-bit tools (most games)
//   <SteamLibrary>/steamapps/common/<Game>/bin/win64/    <- 64-bit tools (Garry's Mod)
//
// Hammer++ ships as hammerplusplus.exe inside those bin folders, next to a
// "hammerplusplus" data folder that holds its scripts and presets. Stock Hammer ships
// as bin/hammer.exe and reads presets from the game folder itself.

#include <filesystem>
#include <string>

enum class HammerEditorType {
	None,
	HammerPlusPlus,
	StockHammer
};

struct HammerInstallInfo {
	HammerEditorType type = HammerEditorType::None;
	std::filesystem::path editorExePath; // The hammer(++).exe that was found
	std::filesystem::path presetFolder;  // Folder where .txt presets should be saved
	bool is64Bit = false;                // Only meaningful when type == HammerPlusPlus
};

namespace HammerDetector {

	// gamePath is a SourceGame::path from SteamDetector. Both gamePath/bin and its
	// parent's bin folder are checked. Returns type == None if no editor is found.
	HammerInstallInfo Detect(const std::string& gamePath);

	// Turns a preset name into a safe lowercase file name stem (no extension),
	// e.g. "Example: Jungle Foliage" -> "example_jungle_foliage".
	std::string NormalizePresetFileName(const std::string& presetName);

	// Resolves the full .txt path a preset should be saved to, creating the destination
	// folder if needed. Returns an empty path if no Hammer installation is found.
	std::filesystem::path ResolvePresetSavePath(const std::string& gamePath, const std::string& presetName);

} // namespace HammerDetector
