#include "headers/ViewportFBO.h" 
#include "headers/portable-file-dialogs.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <imgui_stdlib.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <map>
#include "headers/SprinkleWriter.h"
#include "headers/SprinkleReader.h"
#include "headers/SteamDetector.h"
#include "headers/SourceAssetManager.h" 
#include "headers/ModelRenderer.h"
#include <algorithm>
#include <cmath>
#include <thread>
#include <atomic>

using namespace std;

void test() {
    cout << "Testing Custom Sprinkle Writer...\n" << endl;

    // Build the list of variations (replicating your Jungle Foliage example)
    vector<SprinkleVariation> myVariations;

    myVariations.push_back({ 10, "models/props_island/island_plant01.mdl", "", "1" });
    myVariations.push_back({ 10, "models/props_island/island_plant02.mdl", "0", "1" });
    myVariations.push_back({ 10, "models/props_island/island_plant03.mdl", "1", "1" });
    myVariations.push_back({ 10, "models/props_island/island_plant04.mdl", "", "1" });

    // Generate the text
    string outputText = SprinkleWriter::Generate(
        "Example: Jungle Foliage",
        "prop_static",
        "64 64",
        myVariations
    );

    // Print the perfect format
    cout << outputText << endl;

    string presetName;
    string classname;
    string grid;
    vector<SprinkleVariation> uiVariations;

    cout << "Loading script..." << endl;

    bool success = SprinkleReader::Load("jungle_foliage.txt", presetName, classname, grid, uiVariations);
    if (success) {
        cout << "Successfully loaded: " << presetName << endl;
        cout << "Models found: " << uiVariations.size() << endl;

        // Example: The user edited the first model in the interface (changed chance to 50)
        if (!uiVariations.empty()) {
            uiVariations[0].chance = 50;
            cout << "User changed the first model's chance to 50." << endl;
        }

        // 1. Generate the updated formatted text
        string newOutputText = SprinkleWriter::Generate(presetName, classname, grid, uiVariations);

        // 2. Save it back to the disk (overwriting or creating a new file)
        ofstream outFile("jungle_foliage_edited.txt");

        if (outFile.is_open()) {
            outFile << newOutputText;
            outFile.close();
            cout << "\nSuccess! The edited script was saved to 'jungle_foliage_edited.txt'." << endl;
        }
        else {
            cout << "\nError: Could not open the file to write." << endl;
        }
    }
    else {
        cout << "Could not find or read the file. Check if 'jungle_foliage.txt' exists in the folder." << endl;
    }

    cin.get();
    //return 0;
}

int main() {
    // Game Asset Indexer state variables
    vector<string> indexedModels;
    atomic<bool> isIndexingAssets{false};
    atomic<bool> indexingResultReady{false};
    vector<string> pendingIndexedModels;
    thread indexingThread;
    string searchFilter = "";
    int lastSelectedGameIndex = -1;
    bool gameJustChanged = false; // Set when the selected game changes, so stale model selection can be cleared

    // The model-path autocomplete popup used to scan the entire indexedModels list
    // (tens of thousands of entries) with a substring search on EVERY frame while the
    // input was active, which is the real cause of the perceived "slowness" - it's not
    // the asset indexing (that's cached/fast), it's this per-frame UI filtering.
    // Cache the filtered results per variation slot and only recompute when the typed
    // text (or the underlying model list) actually changes.
    map<size_t, string> suggestionCacheQuery;
    map<size_t, vector<string>> suggestionCacheResults;

    // GLFW and ImGui initialization
    if (!glfwInit()) return -1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    GLFWwindow* window = glfwCreateWindow(1024, 768, "Sprinkle Script Maker", NULL, NULL);
    if (!window) { glfwTerminate(); return -1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    string uiPresetName = "Example: Jungle Foliage";
    string uiClassname = "prop_static";
    string uiGrid = "64 64";
    vector<SprinkleVariation> uiVariations;
    string statusMessage = "Ready."; // To notify if saving was successful

    // Detect Windows DPI/Scale and apply it to the ImGui interface
    float xscale, yscale;
    glfwGetWindowContentScale(window, &xscale, &yscale);
    ImGui::GetStyle().ScaleAllSizes(xscale);
    ImGui::GetIO().FontGlobalScale = xscale; // Increases the base text size to match Windows scale

    // ----------------------------------------------------
    // INITIALIZE STEAM DETECTION
    // ----------------------------------------------------
    vector<SourceGame> detectedGames = SteamDetector::GetInstalledGames();
    int selectedGameIndex = 0; // Keeps track of which game the user chose

    // ----------------------------------------------------
    // 3D PREVIEW VIEWPORT (FBO + orbit camera state)
    // ----------------------------------------------------
    static ViewportFBO previewFBO;
    static bool fboInitialized = false;

    static float cameraYaw = 0.0f;
    static float cameraPitch = 20.0f;
    static float cameraDistance = 150.0f;

    // Real studio model (.mdl/.vtx/.vvd) preview, and the last path we loaded
    // so we only reload/reparse when the selection actually changes.
    static ModelRenderer previewModel;
    static string lastLoadedModelPath;

    // Render the preview at a higher pixel density than the on-screen panel
    // (supersampling), so the image stays crisp. 2.0 = twice the resolution.
    static constexpr float kPreviewResolutionScale = 2.0f;

    // ----------------------------------------------------
    // MAIN LOOP
    // ----------------------------------------------------
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // ----------------------------------------------------
        // NEW CODE: LOCK UI TO THE MAIN WINDOW BORDERS
        // ----------------------------------------------------
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);

        // Setup flags to remove window decorations and lock its position
        ImGuiWindowFlags window_flags = 0;
        window_flags |= ImGuiWindowFlags_NoTitleBar;
        window_flags |= ImGuiWindowFlags_NoCollapse;
        window_flags |= ImGuiWindowFlags_NoResize;
        window_flags |= ImGuiWindowFlags_NoMove;
        window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus;
        window_flags |= ImGuiWindowFlags_NoNavFocus;
        // ----------------------------------------------------

        // Your window now uses the flags to stay "pinned" to the background
        ImGui::Begin("Sprinkle Tool Editor", nullptr, window_flags);

        // --- GAME SELECTION MENU ---
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), "Target Environment");

        if (!detectedGames.empty()) {
            string previewName = detectedGames[selectedGameIndex].name;

            if (ImGui::BeginCombo("Detected Games", previewName.c_str())) {
                for (int n = 0; n < detectedGames.size(); n++) {
                    const bool isSelected = (selectedGameIndex == n);
                    if (ImGui::Selectable(detectedGames[n].name.c_str(), isSelected)) {
                        selectedGameIndex = n;
                    }
                    if (isSelected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            // Trigger asset indexing if selected game changes or on initial boot.
            // Runs on a background thread so the window shows/renders immediately instead
            // of freezing for the whole (potentially slow) scan duration.
            if (selectedGameIndex != lastSelectedGameIndex && !isIndexingAssets.load()) {
                lastSelectedGameIndex = selectedGameIndex;
                gameJustChanged = true; // Model selection/preview state below belongs to the old game and must be reset

                if (indexingThread.joinable()) indexingThread.join(); // Previous scan already finished by now

                isIndexingAssets = true;
                string gameRootPath = detectedGames[selectedGameIndex].path;
                indexingThread = std::thread([gameRootPath, &pendingIndexedModels, &indexingResultReady, &isIndexingAssets]() {
                    auto result = SourceAssetManager::IndexGameModels(gameRootPath);
                    pendingIndexedModels = std::move(result);
                    indexingResultReady = true;
                    isIndexingAssets = false;
                });
            }

            // Pick up the finished background scan's results, if any, without blocking.
            if (indexingResultReady.load()) {
                indexedModels = std::move(pendingIndexedModels);
                indexingResultReady = false;
            }

            if (isIndexingAssets.load()) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Indexing models...");
            }

            ImGui::TextDisabled("Path: %s | Indexed Models: %d",
                detectedGames[selectedGameIndex].path.c_str(),
                (int)indexedModels.size());
        }
        ImGui::Separator();

        // 1. Base Settings
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Base Settings");
        ImGui::InputText("Preset Name", &uiPresetName);
        ImGui::InputText("Classname", &uiClassname);
        ImGui::InputText("Grid Size", &uiGrid);

        ImGui::Separator();

        // 2. Model List (Variations)
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Models & Variations");
        if (ImGui::Button("+ Add New Model")) {
            uiVariations.push_back({ 10, "models/props/new_model.mdl", "", "1" });
        }

        ImGui::Spacing();

        bool hasErrors = false; // Flag to track if everything is correct

        // Loop to draw a box for each model in the list
        for (size_t i = 0; i < uiVariations.size(); ++i) {
            ImGui::PushID(i); // ImGui needs a unique ID to distinguish each block

            // Validate the model path (C++20 feature)
            bool isPathValid = !uiVariations[i].model.empty() && uiVariations[i].model.ends_with(".mdl");
            if (!isPathValid) {
                hasErrors = true; // Block saving if at least one model is invalid
            }

            ImGui::InputInt("Chance", &uiVariations[i].chance);
            ImGui::InputText("Model Path", &uiVariations[i].model);

            // Optional: Draw a popup with matching indexed models if the user types
            if (ImGui::IsItemActive() && !indexedModels.empty() && !uiVariations[i].model.empty()) {
                ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y));
                ImGui::SetNextWindowSize(ImVec2(ImGui::GetItemRectSize().x, 200));

                // Scanning the whole indexedModels list (tens of thousands of entries after
                // indexing a game like TF2) with a substring search on EVERY frame while the
                // input is active is what actually made typing feel "super slow" - not the
                // asset indexing itself. Only recompute the filtered matches when the typed
                // text changes, and reuse the cached result otherwise.
                auto cachedQueryIt = suggestionCacheQuery.find(i);
                if (cachedQueryIt == suggestionCacheQuery.end() || cachedQueryIt->second != uiVariations[i].model) {
                    vector<string> filtered;
                    for (const auto& mdl : indexedModels) {
                        if (mdl.find(uiVariations[i].model) != std::string::npos) {
                            filtered.push_back(mdl);
                            if (filtered.size() > 50) break;
                        }
                    }
                    suggestionCacheQuery[i] = uiVariations[i].model;
                    suggestionCacheResults[i] = std::move(filtered);
                }

                if (ImGui::Begin("##ModelSuggestions", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) {
                    for (const auto& mdl : suggestionCacheResults[i]) {
                        if (ImGui::Selectable(mdl.c_str())) {
                            uiVariations[i].model = mdl; // Fill input text
                            suggestionCacheQuery.erase(i); // Force refresh next time the popup opens
                            suggestionCacheResults.erase(i);

                            // --- NEW: AUTO-DETECT SKINS ---
                            int skinCount = SourceAssetManager::GetSkinCount(mdl);

                            if (skinCount == 1) {
                                uiVariations[i].skin = "0"; // Only 1 skin exists, set to default
                            }
                            else if (skinCount > 1) {
                                // Let user know multiple exist by leaving blank or putting a hint
                                uiVariations[i].skin = "";
                            }
                            // ------------------------------
                        }
                    }
                    ImGui::End();
                }
            }

            // Show a red warning if invalid
            if (!isPathValid) {
                ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "Error: Path must end with '.mdl' and cannot be empty!");
            }

            ImGui::InputText("Skin", &uiVariations[i].skin);
            ImGui::InputText("Ignore Normals", &uiVariations[i].ignorenormals);

            // Button to delete this model from the list
            if (ImGui::Button("Remove Model")) {
                uiVariations.erase(uiVariations.begin() + i);
                ImGui::PopID();
                break; // Stop the loop immediately to prevent a crash
            }

            ImGui::Separator();
            ImGui::PopID();
        }

        ImGui::Spacing();

        // --- MODEL LISTING & SCRIPT EDITOR PANEL ---
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Model Selection & Script Editor");

        static char searchBuffer[128] = "";
        ImGui::InputText("Filter Models", searchBuffer, sizeof(searchBuffer));

        static std::string selectedModelPath = "";
        static int selectedVariationIndex = -1;

        if (gameJustChanged) {
            // The previously selected model/preview belonged to the old game's index,
            // which no longer exists after re-indexing - clear it to avoid stale lookups.
            selectedModelPath.clear();
            gameJustChanged = false;
        }

        if (ImGui::BeginChild("ModelListRegion", ImVec2(0, 180), true, ImGuiWindowFlags_HorizontalScrollbar)) {
            // Snapshot copy so we don't race with the background indexing thread while it
            // publishes a new index (e.g. right after switching games).
            //
            // This used to iterate ALL ~24000 indexed models EVERY FRAME and call
            // GetSkinCount() for each one - which opens the entire VPK archive the first
            // time a given model is seen. Doing that for thousands of models in a single
            // frame is what caused the freeze right after indexing finished. Apply the
            // text filter first (cheap string search) to build a small filtered list, then
            // use ImGuiListClipper so only the handful of rows actually visible on screen
            // ever call GetSkinCount() or get rendered.
            static vector<std::pair<string, string>> filteredModelList;
            static string lastSearchBuffer;
            static size_t lastModelCount = SIZE_MAX;
            size_t currentModelCount = indexedModels.size();
            if (lastSearchBuffer != searchBuffer || lastModelCount != currentModelCount) {
                lastSearchBuffer = searchBuffer;
                lastModelCount = currentModelCount;
                filteredModelList.clear();
                for (const auto& [modelPath, sourcePath] : SourceAssetManager::GetModelLocationsSnapshot()) {
                    if (strlen(searchBuffer) > 0 && modelPath.find(searchBuffer) == std::string::npos) {
                        continue;
                    }
                    filteredModelList.emplace_back(modelPath, sourcePath);
                }
            }

            ImGuiListClipper clipper;
            clipper.Begin((int)filteredModelList.size());
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                    const auto& [modelPath, sourcePath] = filteredModelList[row];
                    ImGui::PushID(row);
                    int skinCount = SourceAssetManager::GetSkinCount(modelPath);
                    std::string label = modelPath + " (Skins: " + std::to_string(skinCount) + ")";

                    if (ImGui::Selectable(label.c_str(), selectedModelPath == modelPath)) {
                        selectedModelPath = modelPath;
                    }
                    ImGui::PopID();
                }
            }
        }
        ImGui::EndChild();

        ImGui::Spacing();
        if (ImGui::Button("Add Selected Model to Script")) {
            if (!selectedModelPath.empty()) {
                int skins = SourceAssetManager::GetSkinCount(selectedModelPath);
                // Inserting with a default chance of 10. Assumes fields: {chance, model, skin, ignorenormals}
                uiVariations.push_back({ 10, selectedModelPath, (skins == 1 ? "0" : ""), "1" });
            }
        }

        ImGui::SameLine();

        // Selector to choose which script entry to override
        ImGui::PushItemWidth(150);
        ImGui::InputInt("Target Entry (Index)", &selectedVariationIndex);
        ImGui::PopItemWidth();

        ImGui::SameLine();

        if (ImGui::Button("Modify Selected Script Entry") && selectedVariationIndex >= 0 && selectedVariationIndex < uiVariations.size()) {
            if (!selectedModelPath.empty()) {
                uiVariations[selectedVariationIndex].model = selectedModelPath;
                int skins = SourceAssetManager::GetSkinCount(selectedModelPath);
                uiVariations[selectedVariationIndex].skin = (skins == 1 ? "0" : "");
            }
        }
        // ---------------------------------------------

        ImGui::Spacing();

        // 3. Save and Load Buttons (Native Windows Dialogs)

        // If there are errors, disable the Save button!
        if (hasErrors) {
            ImGui::BeginDisabled();
        }

        if (ImGui::Button("Save As...", ImVec2(150, 40))) {
            // Open Windows Save Dialog
            auto destination = pfd::save_file("Save Sprinkle Script",
                pfd::path::home() + pfd::path::separator() + "my_preset.txt",
                { "Text Files", "*.txt", "All Files", "*" }).result();

            // If the user didn't click "Cancel"
            if (!destination.empty()) {
                std::string outputText = SprinkleWriter::Generate(uiPresetName, uiClassname, uiGrid, uiVariations);
                std::ofstream outFile(destination); // Save to the chosen path
                if (outFile.is_open()) {
                    outFile << outputText;
                    outFile.close();
                    statusMessage = "Success: File saved successfully!";
                }
                else {
                    statusMessage = "Error: Could not save to the selected location.";
                }
            }
        }

        // If it was disabled, we must end the disabled state here so the next buttons still work
        if (hasErrors) {
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), " Fix errors before saving!");
        }
        else {
            ImGui::SameLine(); // Put the load button on the same line if there's no error text
        }

        if (ImGui::Button("Load File...", ImVec2(150, 40))) {
            // Open Windows Open Dialog
            auto selection = pfd::open_file("Open Sprinkle Script",
                pfd::path::home(),
                { "Text Files", "*.txt", "All Files", "*" },
                pfd::opt::none).result();

            // If the user selected a file
            if (!selection.empty()) {
                std::string filepath = selection[0]; // pfd returns a list, we just want the first file
                bool success = SprinkleReader::Load(filepath, uiPresetName, uiClassname, uiGrid, uiVariations);
                if (success) {
                    statusMessage = "Success: File loaded successfully!";
                }
                else {
                    statusMessage = "Error: Could not read the file format.";
                }
            }
        }

        // Show the status message below the buttons
        ImGui::Spacing();
        ImGui::Text("%s", statusMessage.c_str());

        // ----------------------------------------------------
        // 3D MODEL PREVIEW (embedded inside the main window)
        // ----------------------------------------------------
        if (!fboInitialized) {
            previewFBO.Init(800 * kPreviewResolutionScale, 600 * kPreviewResolutionScale);
            fboInitialized = true;
        }

        // --- RENDER PASS: Render 3D scene to FBO ---
        previewFBO.Bind();
        glViewport(0, 0, previewFBO.width, previewFBO.height);
        glClearColor(0.15f, 0.15f, 0.18f, 1.0f); // Dark gray viewport background
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);

        // --- PROJECTION MATRIX ---
        // The GL context here is created without a "core profile" hint, so the
        // legacy fixed-function pipeline (glMatrixMode/glFrustum/glBegin) is
        // still available directly from opengl32.dll, no extra loader needed.
        {
            float aspect = (previewFBO.height > 0)
                ? (float)previewFBO.width / (float)previewFBO.height
                : 1.0f;
            const float nearPlane = 1.0f;
            const float farPlane = 5000.0f;
            const float fovYDeg = 60.0f;
            float top = nearPlane * tanf(fovYDeg * 0.5f * (3.14159265f / 180.0f));
            float right = top * aspect;

            glMatrixMode(GL_PROJECTION);
            glLoadIdentity();
            glFrustum(-right, right, -top, top, nearPlane, farPlane);

            // --- VIEW (ORBIT CAMERA) MATRIX ---
            glMatrixMode(GL_MODELVIEW);
            glLoadIdentity();
            glTranslatef(0.0f, 0.0f, -cameraDistance);
            glRotatef(cameraPitch, 1.0f, 0.0f, 0.0f);
            glRotatef(cameraYaw, 0.0f, 1.0f, 0.0f);
        }

        // --- MODEL RENDERING ---
        // (Re)load the studio model (.mdl + .vtx + .vvd) whenever the selection
        // changes, then draw the real baked mesh via mdlpp.
        if (selectedModelPath != lastLoadedModelPath) {
            lastLoadedModelPath = selectedModelPath;
            if (selectedModelPath.empty()) {
                previewModel.Clear();
            }
            else if (!previewModel.Load(selectedModelPath)) {
                std::cerr << "[ModelPreview] " << previewModel.lastError << std::endl;
            }
        }

        if (!selectedModelPath.empty()) {
            // Ground grid
            glColor3f(0.4f, 0.4f, 0.4f);
            glBegin(GL_LINES);
            const float gridSize = 100.0f;
            const int gridLines = 10;
            for (int i = -gridLines; i <= gridLines; ++i) {
                float offset = i * (gridSize / gridLines);
                glVertex3f(offset, 0.0f, -gridSize);
                glVertex3f(offset, 0.0f, gridSize);
                glVertex3f(-gridSize, 0.0f, offset);
                glVertex3f(gridSize, 0.0f, offset);
            }
            glEnd();

            if (previewModel.loaded) {
                previewModel.Draw();
            }
            else {
                // Fallback placeholder: mdlpp couldn't parse this model (or its
                // .vtx/.vvd companions weren't found), so draw a wireframe cube
                // instead of showing nothing.
                const float s = 20.0f;
                glColor3f(0.9f, 0.6f, 0.1f);
                glBegin(GL_LINES);
                float verts[8][3] = {
                    { -s, 0.0f, -s }, { s, 0.0f, -s }, { s, 0.0f, s }, { -s, 0.0f, s },
                    { -s, 2 * s, -s }, { s, 2 * s, -s }, { s, 2 * s, s }, { -s, 2 * s, s }
                };
                int edges[12][2] = {
                    {0,1},{1,2},{2,3},{3,0},
                    {4,5},{5,6},{6,7},{7,4},
                    {0,4},{1,5},{2,6},{3,7}
                };
                for (auto& edge : edges) {
                    glVertex3fv(verts[edge[0]]);
                    glVertex3fv(verts[edge[1]]);
                }
                glEnd();
            }
        }

        ViewportFBO::Unbind();

        // --- IMGUI VIEWPORT (embedded child, not a floating window) ---
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "3D Model Preview");

        if (ImGui::BeginChild("ModelPreviewRegion", ImVec2(0, 400), true)) {
            // Get available region size inside the child
            ImVec2 panelSize = ImGui::GetContentRegionAvail();
            if (panelSize.x < 50) panelSize.x = 50;
            if (panelSize.y < 50) panelSize.y = 50;

            // Resize the FBO to a higher resolution than the displayed panel
            // (supersampling) so the preview looks sharp even when scaled down.
            previewFBO.Resize((int)(panelSize.x * kPreviewResolutionScale), (int)(panelSize.y * kPreviewResolutionScale));

            // Display the FBO texture as an image inside the child window
            ImGui::Image((void*)(intptr_t)previewFBO.textureColorBuffer, panelSize, ImVec2(0, 1), ImVec2(1, 0));

            // Mouse interaction: click and drag inside the viewport to rotate camera
            if (ImGui::IsItemHovered() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                ImVec2 delta = ImGui::GetIO().MouseDelta;
                cameraYaw += delta.x * 0.5f;
                cameraPitch += delta.y * 0.5f;

                // Clamp pitch to prevent camera flipping
                if (cameraPitch > 89.0f) cameraPitch = 89.0f;
                if (cameraPitch < -89.0f) cameraPitch = -89.0f;
            }

            // Zoom control via mouse wheel
            if (ImGui::IsItemHovered()) {
                float wheel = ImGui::GetIO().MouseWheel;
                if (wheel != 0.0f) {
                    cameraDistance -= wheel * 10.0f;
                    cameraDistance = std::clamp(cameraDistance, 10.0f, 1000.0f);
                }
            }
        }
        ImGui::EndChild();

        ImGui::End();
        // --- END OF WINDOW DRAWING ---

        // Render to screen 
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.15f, 0.15f, 0.15f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    // Clean shutdown
    // Make sure the background indexing thread (if any) finishes before we tear down state
    // it might still be writing to - joining an already-finished thread returns immediately.
    if (indexingThread.joinable()) indexingThread.join();

    previewFBO.Cleanup();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}