cmake_minimum_required(VERSION 3.15)
project(SprinkleScriptMaker VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(MZ_BZIP2 OFF CACHE BOOL "Disable bzip2" FORCE)
set(MZ_LZMA OFF CACHE BOOL "Disable lzma" FORCE)
set(MZ_ZSTD OFF CACHE BOOL "Disable zstd" FORCE)

include(FetchContent)
message(STATUS "Downloading SourcePP from GitHub...")
FetchContent_Declare(
    sourcepp
    GIT_REPOSITORY https://github.com/craftablescience/sourcepp.git
    GIT_TAG        main
)
FetchContent_MakeAvailable(sourcepp)

add_executable(SprinkleScriptMaker src/main.cpp)

target_link_libraries(SprinkleScriptMaker PRIVATE sourcepp)