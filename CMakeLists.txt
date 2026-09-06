cmake_minimum_required(VERSION 3.15)
project(SprinkleScriptMaker VERSION 1.0 LANGUAGES CXX)

add_compile_definitions(NOMINMAX)
add_compile_definitions(WIN32_LEAN_AND_MEAN)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(MZ_BZIP2 OFF CACHE BOOL "Disable bzip2" FORCE)
set(MZ_LZMA OFF CACHE BOOL "Disable lzma" FORCE)
set(MZ_ZSTD OFF CACHE BOOL "Disable zstd" FORCE)

include(FetchContent)

message(STATUS "Downloading SourcePP...")
FetchContent_Declare(sourcepp GIT_REPOSITORY https://github.com/craftablescience/sourcepp.git GIT_TAG main)
set(SOURCEPP_BUILD_VPKPP ON CACHE BOOL "" FORCE)
set(SOURCEPP_USE_MDLPP ON CACHE BOOL "" FORCE)
set(SOURCEPP_USE_VTFPP ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(sourcepp)

message(STATUS "Downloading GLFW...")
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4)
FetchContent_MakeAvailable(glfw)

message(STATUS "Downloading Dear ImGui...")
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG master)
FetchContent_Populate(imgui)

set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
)

add_executable(SprinkleScriptMaker 
    src/main.cpp
    src/headers/SprinkleWriter.h
    src/headers/SprinkleReader.h
    src/headers/portable-file-dialogs.h
    src/headers/SteamDetector.h
    src/headers/SourceAssetManager.h
    src/headers/ViewportFBO.h
    src/headers/ModelRenderer.h
    src/headers/HammerDetector.h
    ${IMGUI_SOURCES}
)

target_link_libraries(SprinkleScriptMaker PRIVATE sourcepp::vpkpp sourcepp::mdlpp sourcepp::vtfpp sourcepp::kvpp glfw opengl32)

target_include_directories(SprinkleScriptMaker PRIVATE 
    ${sourcepp_SOURCE_DIR}/include
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imgui_SOURCE_DIR}/misc/cpp
    ${CMAKE_BINARY_DIR}/_deps/tsl_hat_trie-src/include
)
