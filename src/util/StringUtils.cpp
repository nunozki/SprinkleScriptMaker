#include "util/StringUtils.h"

namespace StringUtils {

	std::string ExtractQuoted(std::string_view line, std::size_t index) {
		std::size_t searchFrom = 0;
		for (std::size_t i = 0;; ++i) {
			std::size_t open = line.find('"', searchFrom);
			if (open == std::string_view::npos) return {};
			std::size_t close = line.find('"', open + 1);
			if (close == std::string_view::npos) return {};

			if (i == index) {
				return std::string(line.substr(open + 1, close - open - 1));
			}
			searchFrom = close + 1;
		}
	}

	std::string_view Trim(std::string_view line) {
		constexpr std::string_view kWhitespace = " \t\r\n";
		std::size_t first = line.find_first_not_of(kWhitespace);
		if (first == std::string_view::npos) return {};
		std::size_t last = line.find_last_not_of(kWhitespace);
		return line.substr(first, last - first + 1);
	}

	std::string_view StripLineComment(std::string_view line) {
		std::size_t commentPos = line.find("//");
		return commentPos == std::string_view::npos ? line : line.substr(0, commentPos);
	}

} // namespace StringUtils
