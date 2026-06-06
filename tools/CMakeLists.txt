# The loopback harness uses POSIX sockets and is a macOS/Linux dev tool. The
# production sender/receiver get their own platform socket code in M3/M4.
if(WIN32)
    return()
endif()

add_executable(loopback_harness
    loopback_harness.cpp
    wav.cpp
)

target_link_libraries(loopback_harness PRIVATE audiopipe_core)
target_compile_features(loopback_harness PRIVATE cxx_std_17)

if(NOT MSVC)
    target_compile_options(loopback_harness PRIVATE -Wall -Wextra)
endif()

find_package(Threads REQUIRED)
target_link_libraries(loopback_harness PRIVATE Threads::Threads)
