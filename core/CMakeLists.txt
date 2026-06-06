add_library(audiopipe_core
    src/config.cpp
    src/log.cpp
    src/packet.cpp
    src/jitter_buffer.cpp
    src/drift_control.cpp
    src/resampler.cpp
    src/compile_check.cpp
)

target_include_directories(audiopipe_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# libsoxr (variable-rate resampler, spec §3/§8.4). Prefer pkg-config
# (Homebrew on macOS, system on Linux); fall back to find_library, which
# covers vcpkg/manual installs on Windows (vcpkg install soxr).
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(SOXR QUIET IMPORTED_TARGET soxr)
endif()

if(TARGET PkgConfig::SOXR)
    set(AUDIOPIPE_SOXR PkgConfig::SOXR)
else()
    find_path(SOXR_INCLUDE_DIR soxr.h)
    find_library(SOXR_LIBRARY NAMES soxr soxr-lsr)
    if(NOT SOXR_INCLUDE_DIR OR NOT SOXR_LIBRARY)
        message(FATAL_ERROR
            "libsoxr not found. Install it: 'brew install libsoxr' (macOS), "
            "'vcpkg install soxr' (Windows), or your distro's libsoxr-dev.")
    endif()
    add_library(audiopipe_soxr INTERFACE)
    target_include_directories(audiopipe_soxr INTERFACE ${SOXR_INCLUDE_DIR})
    target_link_libraries(audiopipe_soxr INTERFACE ${SOXR_LIBRARY})
    set(AUDIOPIPE_SOXR audiopipe_soxr)
endif()

target_link_libraries(audiopipe_core
    PUBLIC nlohmann_json::nlohmann_json
    PRIVATE ${AUDIOPIPE_SOXR}
)

target_compile_features(audiopipe_core PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(audiopipe_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(audiopipe_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
