#include "platform/HammerDetector.h"

#include <cctype>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

	// The 64-bit tools folder name varies by game: most use "x64", Garry's Mod uses "win64".
	const std::vector<std::string> kX64FolderNames = { "x64", "win64" };
	const std::vector<std::string> kHammerPlusPlusExeNames = { "hammerplusplus.exe", "HammerPlusPlus.exe" };
	const std::vector<std::string> kStockHammerExeNames = { "hammer.exe" };

	// Returns the first `dir/name` that exists, or an empty path.
	fs::path FindExe(const fs::path& dir, const std::vector<std::string>& exeNames) {
		for (const auto& exeName : exeNames) {
			fs::path exePath = dir / exeName;
			if (fs::exists(exePath)) return exePath;
		}
		return {};
	}

} // namespace

namespace HammerDetector {

	HammerInstallInfo Detect(const std::string& gamePath) {
		HammerInstallInfo result;

		fs::path modPath = fs::path(gamePath);
		fs::path gameRoot = modPath.parent_path();

		// Some games keep their tools next to the mod folder, others inside it.
		const std::vector<fs::path> binCandidates = {
			gameRoot / "bin",
			modPath / "bin"
		};

		// 1. 64-bit Hammer++
		for (const auto& binDir : binCandidates) {
			if (!fs::exists(binDir)) continue;
			for (const auto& x64Folder : kX64FolderNames) {
				fs::path x64Dir = binDir / x64Folder;
				fs::path exePath = FindExe(x64Dir, kHammerPlusPlusExeNames);
				if (!exePath.empty()) {
					result.type = HammerEditorType::HammerPlusPlus;
					result.editorExePath = exePath;
					result.is64Bit = true;
					result.presetFolder = x64Dir / "hammerplusplus" / "scripts" / "sprinkle";
					return result;
				}
			}
		}

		// 2. 32-bit Hammer++ (directly inside bin/)
		for (const auto& binDir : binCandidates) {
			if (!fs::exists(binDir)) continue;
			fs::path exePath = FindExe(binDir, kHammerPlusPlusExeNames);
			if (!exePath.empty()) {
				result.type = HammerEditorType::HammerPlusPlus;
				result.editorExePath = exePath;
				result.is64Bit = false;
				result.presetFolder = binDir / "hammerplusplus" / "scripts" / "sprinkle";
				return result;
			}
		}

		// 3. Stock Hammer: normally bin/hammer.exe, but some older mods keep it in the mod folder.
		std::vector<fs::path> stockCandidates = binCandidates;
		stockCandidates.push_back(modPath);

		for (const auto& dir : stockCandidates) {
			if (!fs::exists(dir)) continue;
			fs::path exePath = FindExe(dir, kStockHammerExeNames);
			if (!exePath.empty()) {
				result.type = HammerEditorType::StockHammer;
				result.editorExePath = exePath;
				result.is64Bit = false;
				// Stock Hammer reads presets from the game folder, not from bin/
				result.presetFolder = modPath / "scripts" / "hammer" / "sprinkle";
				return result;
			}
		}

		return result; // type == None
	}

	std::string NormalizePresetFileName(const std::string& presetName) {
		std::string normalized;
		normalized.reserve(presetName.size());

		bool lastWasUnderscore = false;
		for (char c : presetName) {
			unsigned char uc = static_cast<unsigned char>(c);

			if (std::isspace(uc)) {
				if (!lastWasUnderscore && !normalized.empty()) {
					normalized.push_back('_');
					lastWasUnderscore = true;
				}
				continue;
			}

			// Keep letters, digits, underscores and hyphens; drop everything else (":", "!", ...).
			if (std::isalnum(uc) || c == '_' || c == '-') {
				normalized.push_back(static_cast<char>(std::tolower(uc)));
				lastWasUnderscore = false;
			}
		}

		// Trim a trailing underscore left by a trailing space or dropped character.
		while (!normalized.empty() && normalized.back() == '_') {
			normalized.pop_back();
		}

		return normalized.empty() ? "preset" : normalized;
	}

	fs::path ResolvePresetSavePath(const std::string& gamePath, const std::string& presetName) {
		HammerInstallInfo info = Detect(gamePath);
		if (info.type == HammerEditorType::None) {
			return {};
		}

		std::error_code ec;
		fs::create_directories(info.presetFolder, ec);

		return info.presetFolder / (NormalizePresetFileName(presetName) + ".txt");
	}

} // namespace HammerDetector
