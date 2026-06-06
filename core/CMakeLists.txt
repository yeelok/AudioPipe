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

# libsoxr (variable-rate resampler, spec §3/§8.4). macOS/Linux via pkg-config;
# Windows/vcpkg wiring lands in M4.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SOXR REQUIRED IMPORTED_TARGET soxr)

target_link_libraries(audiopipe_core
    PUBLIC nlohmann_json::nlohmann_json
    PRIVATE PkgConfig::SOXR
)

target_compile_features(audiopipe_core PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(audiopipe_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(audiopipe_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
