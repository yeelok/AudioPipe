# Dev tools. The socket-based tools use POSIX sockets and build on macOS/Linux
# only; the production Windows sender gets its own socket code in M4.
if(WIN32)
    return()
endif()

find_package(Threads REQUIRED)

# Shared minimal WAV read/write.
add_library(tools_wav STATIC wav.cpp)
target_include_directories(tools_wav PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Loopback harness: frames -> UDP 127.0.0.1 -> jitter buffer -> WAV, with fault
# injection and a fidelity report.
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
    if(NOT MSVC)
        target_compile_options(${t} PRIVATE -Wall -Wextra)
    endif()
endforeach()
