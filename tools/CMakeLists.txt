# Dev tools (macOS/Linux only; the production Windows sender lives in sender/).
if(WIN32)
    return()
endif()

find_package(Threads REQUIRED)

# Shared minimal WAV read/write.
add_library(tools_wav STATIC wav.cpp)
target_include_directories(tools_wav PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# AOO real-time sender: WAV/sine -> AooSource -> UDP. Stand-in for the Windows
# sender so the receiver + AudioPipe device can be exercised end-to-end on a Mac.
add_executable(aoo_send aoo_send.cpp)
target_link_libraries(aoo_send PRIVATE tools_wav Aoo::aoo Threads::Threads)

target_compile_features(aoo_send PRIVATE cxx_std_17)
if(NOT MSVC)
    target_compile_options(aoo_send PRIVATE -Wall -Wextra)
endif()
