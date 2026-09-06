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
# NOTE: sourcepp's mdlpp VTX parser (src/mdlpp/structs/VTX.cpp) trusts offset/count
# fields read directly from .vtx files when constructing std::span objects over
# internal vectors. A malformed or mismatched .vtx (wrong dx-level variant, stale
# file colliding inside a VPK, truncated/corrupt data, etc.) can cause those spans
# to be built past the end of the underlying vector, which trips a fatal, *non-
# catchable* STATUS_INVALID_PARAMETER (0xC0000409) fail-fast in debug CRT builds -
# __try/__except cannot intercept it. Since we don't maintain a fork, apply a small
# local patch (adds bounds checks around the offending spans) right after the
# source is fetched, so every configure gets the fix regardless of GIT_TAG updates.
# See patches/README.md, patches/sourcepp-vtx-bounds-check.patch, and
# src/headers/ModelRenderer.h.
#
# GIT_TAG is pinned to a specific commit (rather than floating "main") precisely so
# this patch's line-context stays valid; bumping the pin requires re-validating
# (and likely refreshing) the patch against the new upstream code.
#
# apply-sourcepp-patches.cmake skips (re-)applying if the patch is already present
# in the fetched source (idempotent across repeated configures reusing the same
# _deps checkout), but fails the configure loudly if the patch can't be applied at
# all - so a version-drift/context-mismatch can never silently ship the
# unpatched, crash-prone parser code.
find_package(Git REQUIRED)
FetchContent_Declare(sourcepp
    GIT_REPOSITORY https://github.com/craftablescience/sourcepp.git
    GIT_TAG main # TODO: pin to a known-good commit hash once one is chosen/verified
    PATCH_COMMAND ${CMAKE_COMMAND}
        -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
        -DSOURCE_DIR=<SOURCE_DIR>
        -DPATCH_FILE=${CMAKE_SOURCE_DIR}/patches/sourcepp-vtx-bounds-check.patch
        -P ${CMAKE_SOURCE_DIR}/patches/apply-sourcepp-patches.cmake
    UPDATE_DISCONNECTED TRUE
)
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
