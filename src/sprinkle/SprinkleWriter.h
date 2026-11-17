#pragma once

#include <filesystem>
#include <string>

#include "sprinkle/SprinklePreset.h"

namespace SprinkleWriter {

	// Generates the KeyValues text of a Sprinkle preset, in the layout Hammer's Sprinkle tool expects.
	std::string Generate(const SprinklePreset& preset);

	// Generates the preset and writes it to disk, overwriting any existing file.
	// Returns false if the file couldn't be opened for writing.
	bool SaveToFile(const SprinklePreset& preset, const std::filesystem::path& path);

} // namespace SprinkleWriter
