#include <fstream>
#include <algorithm>
#include <string>
#include <vector>

using namespace std;

class SprinkleReader {
public:
    // Helper function to extract text inside quotes ("...")
    static string ExtractQuotedValue(const string& line) {
        size_t firstQuote = line.find_first_of('"');
        if (firstQuote == string::npos) return "";
        size_t secondQuote = line.find_first_of('"', firstQuote + 1);
        if (secondQuote == string::npos) return "";
        return line.substr(firstQuote + 1, secondQuote - firstQuote - 1);
    }

    // Helper function to get the second quoted string in a line (e.g. "model" "path/to/model.mdl")
    static string ExtractSecondQuotedValue(const string& line) {
        size_t firstQuote = line.find_first_of('"');
        if (firstQuote == string::npos) return "";
        size_t secondQuote = line.find_first_of('"', firstQuote + 1);
        if (secondQuote == string::npos) return "";

        size_t thirdQuote = line.find_first_of('"', secondQuote + 1);
        if (thirdQuote == string::npos) return "";
        size_t fourthQuote = line.find_first_of('"', thirdQuote + 1);
        if (fourthQuote == string::npos) return "";

        return line.substr(thirdQuote + 1, fourthQuote - thirdQuote - 1);
    }

    static bool Load(const string& filepath,
        string& outPresetName,
        string& outBaseClassname,
        string& outBaseGrid,
        vector<SprinkleVariation>& outVariations)
    {
        ifstream file(filepath);
        if (!file.is_open()) return false;

        string line;
        bool inBaseBlock = false;
        bool inVariationBlock = false;
        SprinkleVariation currentVariation;
        int currentChance = 0;

        // Clear old data before loading the new file
        outVariations.clear();
        outPresetName = "";
        outBaseClassname = "prop_static";
        outBaseGrid = "64 64";

        while (getline(file, line)) {
            // 1. Remove comments (everything after //)
            size_t commentPos = line.find("//");
            if (commentPos != string::npos) {
                line = line.substr(0, commentPos);
            }

            // 2. If it is the first line with the Preset name
            if (outPresetName.empty() && line.find('"') != string::npos) {
                outPresetName = ExtractQuotedValue(line);
                continue;
            }

            // 3. Detect which block we are in
            if (line.find("base") != string::npos) {
                inBaseBlock = true;
                continue;
            }

            // If an isolated number is found, it is the chance of a new variation (e.g., 10)
            if (!inBaseBlock && !inVariationBlock && line.find_first_of("0123456789") != string::npos) {
                // Try to extract the number
                size_t numStart = line.find_first_of("0123456789");
                size_t numEnd = line.find_first_not_of("0123456789", numStart);
                string numStr = line.substr(numStart, numEnd - numStart);
                currentChance = stoi(numStr);
                continue;
            }

            if (line.find("{") != string::npos && currentChance > 0) {
                inVariationBlock = true;
                currentVariation = SprinkleVariation(); // Reset temporary variation
                currentVariation.chance = currentChance;
                continue;
            }

            // 4. Read properties depending on the block
            if (inBaseBlock) {
                if (line.find("\"classname\"") != string::npos) outBaseClassname = ExtractSecondQuotedValue(line);
                if (line.find("grid") != string::npos) outBaseGrid = ExtractQuotedValue(line);
                if (line.find("}") != string::npos) inBaseBlock = false;
            }
            else if (inVariationBlock) {
                if (line.find("\"model\"") != string::npos) currentVariation.model = ExtractSecondQuotedValue(line);
                if (line.find("\"skin\"") != string::npos) currentVariation.skin = ExtractSecondQuotedValue(line);
                if (line.find("\"ignorenormals\"") != string::npos) currentVariation.ignorenormals = ExtractSecondQuotedValue(line);

                // When the block closes, save the variation to the final list
                if (line.find("}") != string::npos) {
                    outVariations.push_back(currentVariation);
                    inVariationBlock = false;
                    currentChance = 0; // Reset
                }
            }
        }

        return true;
    }
};