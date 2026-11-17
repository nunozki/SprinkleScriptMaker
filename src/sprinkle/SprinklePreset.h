#pragma once

#include <string>
#include <vector>

// One weighted entry of a Sprinkle preset (a numbered block in the .txt file).
struct SprinkleVariation {
	int chance = 0;
	std::string model;
	std::string skin;          // Optional: left out of the file when empty
	std::string ignorenormals; // Optional: left out of the file when empty
};

// A whole Sprinkle preset: the root name, the "base" block and its weighted variations.
struct SprinklePreset {
	std::string name;
	std::string classname = "prop_static";
	std::string grid = "64 64";
	std::vector<SprinkleVariation> variations;
};
