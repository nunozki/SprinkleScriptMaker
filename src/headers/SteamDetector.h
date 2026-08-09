#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace std;

// Structure to hold data about a detected Source Engine game
struct SourceGame {
    string name;
    int appId;
    string path;
};

class SteamDetector {
public:
    // Main function to find installed Source Engine games across all Steam libraries
    static vector<SourceGame> GetInstalledGames() {
        vector<SourceGame> installedGames;

        // 1. Get main Steam installation path from Windows Registry
        string steamPath = GetSteamPathFromRegistry();
        if (steamPath.empty()) return installedGames;

        // 2. Get all library folders (external drives, etc.)
        vector<string> libraries = GetLibraryFolders(steamPath);
        libraries.push_back(steamPath); // Include the main Steam folder as well

        // 3. Define the specific Source Engine games we want to look for
        // AppID 440: Team Fortress 2
        // AppID 240: Counter-Strike: Source
        // AppID 4000: Garry's Mod
        // NOTE: Counter-Strike 2 (AppID 730) is intentionally excluded - it runs on
        // Source 2, which the Sprinkle tool doesn't target (only CS:GO/Source 1 did).
        vector<SourceGame> targetGames = {
            {"Team Fortress 2", 440, ""},
            {"Counter-Strike: Source", 240, ""},
            {"Garry's Mod", 4000, ""}
        };

        // 4. Check each library for the specific game manifests
        for (const auto& lib : libraries) {
            for (auto& game : targetGames) {
                // Steam uses appmanifest_<AppID>.acf to mark a game as installed
                filesystem::path manifestPath = filesystem::path(lib) / "steamapps" / ("appmanifest_" + to_string(game.appId) + ".acf");

                if (filesystem::exists(manifestPath)) {
                    SourceGame foundGame = game;

                    // Read the real install folder name from the manifest's "installdir" field.
                    // We can't assume it matches the display name - e.g. Garry's Mod is installed
                    // to "steamapps/common/GarrysMod", not "steamapps/common/Garry's Mod".
                    string installDir = GetInstallDirFromManifest(manifestPath);
                    if (installDir.empty()) installDir = game.name; // Fallback, in case parsing fails

                    // Build the path to the actual game directory (e.g., steamapps/common/GarrysMod)
                    foundGame.path = (filesystem::path(lib) / "steamapps" / "common" / installDir).string();

                    // Prevent duplicates just in case
                    bool alreadyAdded = false;
                    for (const auto& ig : installedGames) {
                        if (ig.appId == game.appId) alreadyAdded = true;
                    }

                    if (!alreadyAdded) {
                        installedGames.push_back(foundGame);
                    }
                }
            }
        }
        return installedGames;
    }

private:
    // Helper: Reads the "installdir" field out of an appmanifest_<AppID>.acf file.
    // This is the actual folder name Steam uses under steamapps/common, which can
    // differ from the game's display name (e.g. Garry's Mod -> "GarrysMod").
    static string GetInstallDirFromManifest(const filesystem::path& manifestPath) {
        ifstream file(manifestPath);
        if (!file.is_open()) return "";

        string line;
        while (getline(file, line)) {
            if (line.find("\"installdir\"") != string::npos) {
                size_t firstQuote = line.find_first_of('"');
                size_t secondQuote = line.find_first_of('"', firstQuote + 1);
                size_t thirdQuote = line.find_first_of('"', secondQuote + 1);
                size_t fourthQuote = line.find_first_of('"', thirdQuote + 1);

                if (thirdQuote != string::npos && fourthQuote != string::npos) {
                    return line.substr(thirdQuote + 1, fourthQuote - thirdQuote - 1);
                }
            }
        }
        return "";
    }

    // Helper: Reads the Windows Registry to find where Steam is installed
    static string GetSteamPathFromRegistry() {
        HKEY hKey;
        // Open the Steam registry key
        if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            char path[MAX_PATH];
            DWORD pathLen = sizeof(path);

            // Read the SteamPath value
            if (RegQueryValueExA(hKey, "SteamPath", nullptr, nullptr, (LPBYTE)path, &pathLen) == ERROR_SUCCESS) {
                RegCloseKey(hKey);

                // Steam registry usually stores paths with forward slashes, replace them with backslashes
                string result(path);
                for (char& c : result) {
                    if (c == '/') c = '\\';
                }
                return result;
            }
            RegCloseKey(hKey);
        }
        return "";
    }

    // Helper: Parses libraryfolders.vdf to find secondary game drives
    static vector<string> GetLibraryFolders(const string& steamPath) {
        vector<string> folders;
        filesystem::path vdfPath = filesystem::path(steamPath) / "steamapps" / "libraryfolders.vdf";

        ifstream file(vdfPath);
        if (!file.is_open()) return folders;

        string line;
        while (getline(file, line)) {
            // Looking for the "path" key in the VDF file
            if (line.find("\"path\"") != string::npos) {
                size_t firstQuote = line.find_first_of('"');
                size_t secondQuote = line.find_first_of('"', firstQuote + 1);
                size_t thirdQuote = line.find_first_of('"', secondQuote + 1);
                size_t fourthQuote = line.find_first_of('"', thirdQuote + 1);

                if (thirdQuote != string::npos && fourthQuote != string::npos) {
                    string path = line.substr(thirdQuote + 1, fourthQuote - thirdQuote - 1);

                    // VDF files often double-escape backslashes (e.g., C:\\SteamLibrary)
                    size_t pos = 0;
                    while ((pos = path.find("\\\\", pos)) != string::npos) {
                        path.replace(pos, 2, "\\");
                        pos += 1;
                    }
                    folders.push_back(path);
                }
            }
        }
        return folders;
    }
};