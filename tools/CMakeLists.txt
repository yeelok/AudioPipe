# The loopback harness uses POSIX sockets and is a macOS/Linux dev tool. The
# production sender/receiver get their own platform socket code in M3/M4.
if(WIN32)
    return()
endif()

find_package(Threads REQUIRED)

add_executable(loopback_harness
    loopback_harness.cpp
    wav.cpp
)
target_link_libraries(loopback_harness PRIVATE audiopipe_core Threads::Threads)
target_compile_features(loopback_harness PRIVATE cxx_std_17)

# Drift-correction simulation (no sockets; pure computation).
add_executable(drift_sim drift_sim.cpp)
target_link_libraries(drift_sim PRIVATE audiopipe_core)
target_compile_features(drift_sim PRIVATE cxx_std_17)

if(NOT MSVC)
    target_compile_options(loopback_harness PRIVATE -Wall -Wextra)
    target_compile_options(drift_sim PRIVATE -Wall -Wextra)
endif()
