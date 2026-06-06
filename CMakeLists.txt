cmake_minimum_required(VERSION 3.20)
project(AudioPipe VERSION 0.1.0 LANGUAGES CXX)

# --- Standard --------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# CMake 4 removed compatibility with cmake_minimum_required < 3.5; some
# FetchContent dependencies still declare an older floor. Raise it for them.
if(NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()

option(AUDIOPIPE_BUILD_TESTS "Build the portable-core unit tests" ON)

include(FetchContent)

# --- Dependencies ----------------------------------------------------------
# JSON config (spec §5.5 / §6.5). Header-only.
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# --- Portable core ---------------------------------------------------------
add_subdirectory(core)

# Dev tools (loopback harness; latency probe later). POSIX-only for now.
add_subdirectory(tools)

# macOS receiver process (built only on Apple platforms).
add_subdirectory(receiver)

# Windows sender (built only on Windows).
add_subdirectory(sender)

# The driver forks build via their own native toolchains, not CMake:
#   driver-mac/ (BlackHole fork)            -> driver-mac/build_driver.sh (Xcode)
#   driver-win/ (Virtual-Audio-Driver fork) -> WDK/Visual Studio, see driver-win/README.md

# --- Tests -----------------------------------------------------------------
if(AUDIOPIPE_BUILD_TESTS)
    enable_testing()
    FetchContent_Declare(Catch2
        URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.7.1.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    add_subdirectory(tests)
endif()
