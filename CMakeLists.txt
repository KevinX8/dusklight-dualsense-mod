cmake_minimum_required(VERSION 3.26)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (NOT DEFINED CMAKE_BUILD_TYPE AND NOT DEFINED ENV{CMAKE_BUILD_TYPE})
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type")
endif ()

project(dualsense_mod VERSION 1.0.0 LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchDusklight.cmake")
add_subdirectory("${DUSKLIGHT_DIR}/sdk" dusklight-sdk EXCLUDE_FROM_ALL)

# SDL3 headers only: the game's own SDL is used at runtime (resolved dynamically, see sdl_dyn.hpp).
find_path(SDL3_INCLUDE_DIR SDL3/SDL.h)
if (NOT SDL3_INCLUDE_DIR)
    include(FetchContent)
    FetchContent_Declare(sdl3_headers
            URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.10/SDL3-3.4.10.tar.gz
            DOWNLOAD_EXTRACT_TIMESTAMP FALSE
            SOURCE_SUBDIR no-build)
    FetchContent_MakeAvailable(sdl3_headers)
    set(SDL3_INCLUDE_DIR "${sdl3_headers_SOURCE_DIR}/include")
endif ()

configure_file(mod.json.in "${CMAKE_CURRENT_BINARY_DIR}/mod.json" @ONLY NEWLINE_STYLE LF)

add_mod(dualsense_mod
        FEATURES game fmt
        SOURCES src/mod.cpp src/settings.cpp src/haptics.cpp src/controller.cpp src/sdl_dyn.cpp
        MOD_JSON "${CMAKE_CURRENT_BINARY_DIR}/mod.json"
)
# DuskDsp.hpp (audio engine internals) lives under src/.
target_include_directories(dualsense_mod PRIVATE "${SDL3_INCLUDE_DIR}" "${DUSKLIGHT_DIR}/src")
if (UNIX)
    target_link_libraries(dualsense_mod PRIVATE ${CMAKE_DL_LIBS})
endif ()

add_executable(ds5_test EXCLUDE_FROM_ALL tests/ds5_test.cpp)
target_include_directories(ds5_test PRIVATE src)
