# Dev tools.
#
# Portable tools (no sockets) build on every OS, including Windows:
#   tools_wav        - minimal WAV read/write
#   pipeline_check   - run a WAV through the full receiver DSP pipeline offline
#
# The socket-based tools use POSIX sockets and build on macOS/Linux only; the
# production Windows sender has its own platform socket code (sender/).

# Shared minimal WAV read/write (portable).
add_library(tools_wav STATIC wav.cpp)
target_include_directories(tools_wav PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Offline full-pipeline check: jitter buffer -> drift resampler -> ring ->
# consumer, on real audio, with clock-drift / loss / reorder injection. No
# sockets, no audio device -> builds and runs on Windows and macOS.
add_executable(pipeline_check pipeline_check.cpp)
target_link_libraries(pipeline_check PRIVATE audiopipe_core tools_wav)
target_compile_features(pipeline_check PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(pipeline_check PRIVATE /W4 /permissive-)
    target_compile_definitions(pipeline_check PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
    target_compile_options(pipeline_check PRIVATE -Wall -Wextra)
endif()

# Socket-based dev tools: POSIX (macOS/Linux) only.
if(NOT WIN32)
    find_package(Threads REQUIRED)

    # Loopback harness: frames -> UDP 127.0.0.1 -> jitter buffer -> WAV, with
    # fault injection and a fidelity report.
    add_executable(loopback_harness loopback_harness.cpp)
    target_link_libraries(loopback_harness PRIVATE audiopipe_core tools_wav Threads::Threads)

    # Real-time §7 packet sender (stand-in for the Windows sender): WAV/sine -> UDP.
    add_executable(udp_send udp_send.cpp)
    target_link_libraries(udp_send PRIVATE audiopipe_core tools_wav)

    # Drift-correction simulation (no sockets; pure computation).
    add_executable(drift_sim drift_sim.cpp)
    target_link_libraries(drift_sim PRIVATE audiopipe_core)

    foreach(t loopback_harness udp_send drift_sim)
        target_compile_features(${t} PRIVATE cxx_std_17)
        target_compile_options(${t} PRIVATE -Wall -Wextra)
    endforeach()
endif()
