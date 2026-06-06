# Windows sender: WASAPI loopback capture -> §7 packets -> UDP. Windows only.
if(NOT WIN32)
    return()
endif()

add_executable(audiopipe_sender
    main.cpp
    service.cpp
    sender_app.cpp
    wasapi_capture.cpp
    udp_sender.cpp
)

target_link_libraries(audiopipe_sender PRIVATE
    audiopipe_core
    ole32        # COM (MMDeviceEnumerator)
    propsys      # PropVariantClear
    ws2_32       # Winsock
    avrt         # (reserved for MMCSS thread prioritization)
)

target_compile_features(audiopipe_sender PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(audiopipe_sender PRIVATE /W4 /permissive-)
    target_compile_definitions(audiopipe_sender PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()
