#include "sprinkle/SprinkleReader.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <string>
#include <string_view>

#include "util/StringUtils.h"

namespace {

	bool IEquals(std::string_view a, std::string_view b) {
		return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
			return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
		});
	}

	// The key of a "key value" line. Keys may be quoted ("model") or bare (grid, base).
	std::string FirstToken(std::string_view line) {
		if (line.empty()) return {};
		if (line.front() == '"') return StringUtils::ExtractQuoted(line, 0);

		std::size_t end = line.find_first_of(" \t{\"");
		return std::string(line.substr(0, end));
	}

	// The value of a "key value" line, whether or not the key itself is quoted.
	std::string LineValue(std::string_view line) {
		bool keyIsQuoted = !line.empty() && line.front() == '"';
		return StringUtils::ExtractQuoted(line, keyIsQuoted ? 1 : 0);
	}

} // namespace

namespace SprinkleReader {

	bool Load(const std::filesystem::path& filepath, SprinklePreset& outPreset) {
		std::ifstream file(filepath);
		if (!file.is_open()) return false;

		SprinklePreset preset;
		bool nameRead = false;
		bool inBaseBlock = false;
		bool inVariationBlock = false;
		int pendingChance = 0;
		SprinkleVariation currentVariation;

		std::string rawLine;
		while (std::getline(file, rawLine)) {
			std::string_view line = StringUtils::Trim(StringUtils::StripLineComment(rawLine));
			if (line.empty()) continue;

			// 1. The first quoted string in the file is the preset name.
			if (!nameRead) {
				if (line.find('"') != std::string_view::npos) {
					preset.name = StringUtils::ExtractQuoted(line, 0);
					nameRead = true;
				}
				continue;
			}

			// 2. Outside any block: either the start of "base" or a variation's chance number.
			if (!inBaseBlock && !inVariationBlock) {
				if (IEquals(FirstToken(line), "base")) {
					inBaseBlock = true;
					continue;
				}

				if (std::isdigit(static_cast<unsigned char>(line.front()))) {
					int chance = 0;
					std::from_chars(line.data(), line.data() + line.size(), chance);
					pendingChance = chance;

					// The opening brace may share the line with the number ("10 {").
					if (line.find('{') == std::string_view::npos) continue;
				}

				if (pendingChance > 0 && line.find('{') != std::string_view::npos) {
					inVariationBlock = true;
					currentVariation = SprinkleVariation{};
					currentVariation.chance = pendingChance;
				}
				continue;
			}

			// 3. Inside a block: read the keys we know about.
			std::string key = FirstToken(line);

			if (inBaseBlock) {
				if (IEquals(key, "classname")) preset.classname = LineValue(line);
				else if (IEquals(key, "grid")) preset.grid = LineValue(line);

				if (line.find('}') != std::string_view::npos) inBaseBlock = false;
			}
			else {
				if (IEquals(key, "model")) currentVariation.model = LineValue(line);
				else if (IEquals(key, "skin")) currentVariation.skin = LineValue(line);
				else if (IEquals(key, "ignorenormals")) currentVariation.ignorenormals = LineValue(line);

				if (line.find('}') != std::string_view::npos) {
					preset.variations.push_back(currentVariation);
					inVariationBlock = false;
					pendingChance = 0;
				}
			}
		}

		outPreset = std::move(preset);
		return true;
	}

} // namespace SprinkleReader
