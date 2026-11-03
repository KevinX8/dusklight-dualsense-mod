cmake_minimum_required(VERSION 3.26)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (NOT DEFINED CMAKE_BUILD_TYPE AND NOT DEFINED ENV{CMAKE_BUILD_TYPE})
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type")
endif ()

project(dualsense_mod VERSION 1.0.0 LANGUAGES CXX)

# Dusklight commit the mod builds against when no local checkout is given (-DDUSKLIGHT_DIR=...).
if (NOT DUSKLIGHT_VERSION)
    set(DUSKLIGHT_VERSION "77705f8bb95ef7b7dca4675b149c4bcb6a48da34")
endif ()
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

# Windows/macOS: the game doesn't export SDL, so the mod ships its own SDL3 library and runs a
# private instance for the DualSense (see src/sdl_dyn.hpp). Linux uses the game's SDL.
set(DS_SDL3_RUNTIME "" CACHE FILEPATH "SDL3 shared library to ship beside the mod (Windows/macOS)")
if (WIN32 AND NOT DS_SDL3_RUNTIME)
    include(FetchContent)
    FetchContent_Declare(sdl3_vc
            URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.10/SDL3-devel-3.4.10-VC.zip
            DOWNLOAD_EXTRACT_TIMESTAMP FALSE
            SOURCE_SUBDIR no-build)
    FetchContent_MakeAvailable(sdl3_vc)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _ds_arch)
    if (_ds_arch MATCHES "arm64|aarch64")
        set(_ds_arch arm64)
    else ()
        set(_ds_arch x64)
    endif ()
    set(DS_SDL3_RUNTIME "${sdl3_vc_SOURCE_DIR}/lib/${_ds_arch}/SDL3.dll")
elseif (APPLE AND NOT DS_SDL3_RUNTIME)
    message(WARNING "DS_SDL3_RUNTIME not set: the mod will look for a system libSDL3 (e.g. Homebrew) "
            "at runtime. Pass -DDS_SDL3_RUNTIME=/path/to/libSDL3.0.dylib to ship one.")
endif ()
set(_ds_runtime_args)
if (DS_SDL3_RUNTIME)
    set(_ds_runtime_args RUNTIME_LIBRARIES "${DS_SDL3_RUNTIME}")
endif ()

configure_file(mod.json.in "${CMAKE_CURRENT_BINARY_DIR}/mod.json" @ONLY NEWLINE_STYLE LF)

add_mod(dualsense_mod
        FEATURES game fmt
        SOURCES src/mod.cpp src/settings.cpp src/haptics.cpp src/controller.cpp src/sdl_dyn.cpp
        MOD_JSON "${CMAKE_CURRENT_BINARY_DIR}/mod.json"
        ${_ds_runtime_args}
)
# DuskDsp.hpp (audio engine internals) lives under src/.
target_include_directories(dualsense_mod PRIVATE "${SDL3_INCLUDE_DIR}" "${DUSKLIGHT_DIR}/src")
if (MSVC)
    # Without it MSVC reports __cplusplus as 199711L and dolphin/types.h #defines nullptr as 0,
    # which breaks the standard headers included after it.
    target_compile_options(dualsense_mod PRIVATE /Zc:__cplusplus)
endif ()
if (UNIX)
    target_link_libraries(dualsense_mod PRIVATE ${CMAKE_DL_LIBS})
endif ()

add_executable(ds5_test EXCLUDE_FROM_ALL tests/ds5_test.cpp)
target_include_directories(ds5_test PRIVATE src)

add_executable(sdl_dyn_test EXCLUDE_FROM_ALL tests/sdl_dyn_test.cpp src/sdl_dyn.cpp)
target_include_directories(sdl_dyn_test PRIVATE src "${SDL3_INCLUDE_DIR}")
target_link_libraries(sdl_dyn_test PRIVATE ${CMAKE_DL_LIBS})

add_executable(tone_test EXCLUDE_FROM_ALL tests/tone_test.cpp src/sdl_dyn.cpp)
target_include_directories(tone_test PRIVATE src "${SDL3_INCLUDE_DIR}")
target_link_libraries(tone_test PRIVATE ${CMAKE_DL_LIBS})
