# macOS receiver process: UDP -> core -> AudioPipe (BlackHole-fork) device.
if(NOT APPLE)
    return()
endif()

add_executable(audiopipe_receiver main.cpp)
target_link_libraries(audiopipe_receiver PRIVATE audiopipe_core)
target_compile_features(audiopipe_receiver PRIVATE cxx_std_17)
target_compile_options(audiopipe_receiver PRIVATE -Wall -Wextra)

target_link_libraries(audiopipe_receiver PRIVATE
    "-framework CoreAudio"
    "-framework AudioToolbox"
    "-framework CoreFoundation"
)
