#include "sprinkle/SprinkleWriter.h"

#include <fstream>
#include <sstream>

namespace SprinkleWriter {

	std::string Generate(const SprinklePreset& preset) {
		std::ostringstream out;

		// Root
		out << "\"" << preset.name << "\"\n";
		out << "{\n";

		// Base block
		out << "\tbase\n";
		out << "\t{\n";
		out << "\t\t\"classname\" \"" << preset.classname << "\"\n";
		out << "\t\tgrid \"" << preset.grid << "\"\n";
		out << "\t}\n";

		// Variations
		for (const auto& var : preset.variations) {
			out << "\n\t" << var.chance << "\n";
			out << "\t{\n";
			out << "\t\t\"model\" \"" << var.model << "\"\n";

			// Optional keys are only written when set
			if (!var.skin.empty()) {
				out << "\t\t\"skin\" \"" << var.skin << "\"\n";
			}
			if (!var.ignorenormals.empty()) {
				out << "\t\t\"ignorenormals\" \"" << var.ignorenormals << "\"\n";
			}

			out << "\t}\n";
		}

		// Close root
		out << "}\n";

		return out.str();
	}

	bool SaveToFile(const SprinklePreset& preset, const std::filesystem::path& path) {
		std::ofstream outFile(path);
		if (!outFile.is_open()) return false;

		outFile << Generate(preset);
		return static_cast<bool>(outFile);
	}

} // namespace SprinkleWriter
