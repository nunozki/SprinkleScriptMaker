#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// Small helpers for the line-based KeyValues-style text files this project reads
// (Sprinkle presets, Steam's .acf/.vdf manifests, Garry's Mod's mount.cfg).
namespace StringUtils {

	// Returns the contents of the index-th (0-based) double-quoted token on a line, e.g.
	//   ExtractQuoted(R"("model" "models/foo.mdl")", 0) == "model"
	//   ExtractQuoted(R"("model" "models/foo.mdl")", 1) == "models/foo.mdl"
	// Returns an empty string if the line has fewer than index + 1 quoted tokens.
	std::string ExtractQuoted(std::string_view line, std::size_t index);

	// Returns the line with leading and trailing whitespace removed.
	std::string_view Trim(std::string_view line);

	// Returns the line with everything from the first "//" onwards removed.
	std::string_view StripLineComment(std::string_view line);

} // namespace StringUtils
