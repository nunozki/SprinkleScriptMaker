#include <iostream>
#include <string>
#include <vector>
#include <sstream>

using namespace std;

// 1. Structure to hold each variation's data
struct SprinkleVariation {
    int chance = 0;
    string model;
    string skin = "";          // Optional, leave empty if not used
    string ignorenormals = ""; // Optional, leave empty if not used
};

// 2. The Custom Writer Class
class SprinkleWriter {
public:
    static string Generate(const string& presetName,
        const string& baseClassname,
        const string& baseGrid,
        const vector<SprinkleVariation>& variations)
    {
        ostringstream out;

        // Root
        out << "\"" << presetName << "\"\n";
        out << "{\n";

        // Base block
        out << "\tbase\n";
        out << "\t{\n";
        out << "\t\t\"classname\" \"" << baseClassname << "\"\n";
        out << "\t\tgrid \"" << baseGrid << "\"\n";
        out << "\t}\n";

        // Variations loop
        for (const auto& var : variations) {
            out << "\n\t" << var.chance << "\n";
            out << "\t{\n";
            out << "\t\t\"model\" \"" << var.model << "\"\n";

            // Only add optional keys if they are not empty
            if (!var.skin.empty()) {
                out << "\t\t\"skin\" \"" << var.skin << "\"\n";
            }
            if (!var.ignorenormals.empty()) {
                out << "\t\t\"ignorenormals\" \"" << var.ignorenormals << "\"\n";
            }

            out << "\t}\n";
        }

        // Close Root
        out << "}\n";

        return out.str();
    }
};