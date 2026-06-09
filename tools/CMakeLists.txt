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

# pipeline_check: run a WAV through the full AOO pipeline offline (in-process
# AooSource -> AooSink, synthetic clock) with drop/reorder/drift injection and a
# listenable output WAV. No sockets/hardware.
add_executable(pipeline_check pipeline_check.cpp)
target_link_libraries(pipeline_check PRIVATE tools_wav Aoo::aoo)

foreach(t aoo_send pipeline_check)
    target_compile_features(${t} PRIVATE cxx_std_17)
    if(NOT MSVC)
        target_compile_options(${t} PRIVATE -Wall -Wextra)
    endif()
endforeach()
