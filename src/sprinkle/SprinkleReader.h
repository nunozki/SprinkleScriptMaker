#pragma once

#include <filesystem>

#include "sprinkle/SprinklePreset.h"

namespace SprinkleReader {

	// Parses a Sprinkle preset .txt file into outPreset.
	// Returns false (leaving outPreset untouched) if the file couldn't be opened.
	//
	// The parser is line based: it expects one key/value pair, brace or chance
	// number per line, which is the layout the Sprinkle tool itself uses.
	// Only "classname", "grid", "model", "skin" and "ignorenormals" are read.
	bool Load(const std::filesystem::path& filepath, SprinklePreset& outPreset);

} // namespace SprinkleReader
