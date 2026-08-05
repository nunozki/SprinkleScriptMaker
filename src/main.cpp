#include <iostream>
#include "headers/SprinkleWriter.h"
#include "headers/SprinkleReader.h"

int main() {
    std::cout << "Testing Custom Sprinkle Writer...\n" << std::endl;

    // Build the list of variations (replicating your Jungle Foliage example)
    std::vector<SprinkleVariation> myVariations;

    myVariations.push_back({ 10, "models/props_island/island_plant01.mdl", "", "1" });
    myVariations.push_back({ 10, "models/props_island/island_plant02.mdl", "0", "1" });
    myVariations.push_back({ 10, "models/props_island/island_plant03.mdl", "1", "1" });
    myVariations.push_back({ 10, "models/props_island/island_plant04.mdl", "", "1" });

    // Generate the text
    std::string outputText = SprinkleWriter::Generate(
        "Example: Jungle Foliage",
        "prop_static",
        "64 64",
        myVariations
    );

    // Print the perfect format
    std::cout << outputText << std::endl;

    std::string presetName;
    std::string classname;
    std::string grid;
    std::vector<SprinkleVariation> uiVariations;

    std::cout << "Loading script..." << std::endl;

    bool success = SprinkleReader::Load("jungle_foliage.txt", presetName, classname, grid, uiVariations);
    if (success) {
        std::cout << "Successfully loaded: " << presetName << std::endl;
        std::cout << "Models found: " << uiVariations.size() << std::endl;

        // Example: The user edited the first model in the interface (changed chance to 50)
        if (!uiVariations.empty()) {
            uiVariations[0].chance = 50;
            std::cout << "User changed the first model's chance to 50." << std::endl;
        }

        // 1. Generate the updated formatted text
        std::string newOutputText = SprinkleWriter::Generate(presetName, classname, grid, uiVariations);

        // 2. Save it back to the disk (overwriting or creating a new file)
        std::ofstream outFile("jungle_foliage_edited.txt");

        if (outFile.is_open()) {
            outFile << newOutputText;
            outFile.close();
            std::cout << "\nSuccess! The edited script was saved to 'jungle_foliage_edited.txt'." << std::endl;
        }
        else {
            std::cout << "\nError: Could not open the file to write." << std::endl;
        }
    }
    else {
        std::cout << "Could not find or read the file. Check if 'jungle_foliage.txt' exists in the folder." << std::endl;
    }

    std::cin.get();
    return 0;
}