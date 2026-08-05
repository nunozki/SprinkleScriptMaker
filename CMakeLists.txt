cmake_minimum_required(VERSION 3.15)
project(SprinkleScriptMaker VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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