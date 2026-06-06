include(Catch)

add_executable(core_tests
    test_sanity.cpp
    test_config.cpp
    test_packet.cpp
    test_ring_buffer.cpp
    test_jitter_buffer.cpp
    test_drift.cpp
)

target_link_libraries(core_tests
    PRIVATE audiopipe_core Catch2::Catch2WithMain
)

catch_discover_tests(core_tests)
