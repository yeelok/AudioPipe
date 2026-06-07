add_library(audiopipe_core
    src/config.cpp
    src/log.cpp
    src/compile_check.cpp
)

target_include_directories(audiopipe_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Note: the variable-rate resampler (libsoxr) and the hand-rolled packet/jitter/
# drift modules were removed when AudioPipe adopted AOO (deps/aoo), which owns
# the wire protocol, jitter buffer, time-DLL clock sync and dynamic resampling.
# core/ is now just config + logging + format constants + a lock-free ring.
target_link_libraries(audiopipe_core
    PUBLIC nlohmann_json::nlohmann_json
)

target_compile_features(audiopipe_core PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(audiopipe_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(audiopipe_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
