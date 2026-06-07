include(Catch)

find_package(Threads REQUIRED)

add_executable(core_tests
    test_sanity.cpp
    test_config.cpp
    test_ring_buffer.cpp
    test_aoo_loopback.cpp
)

target_link_libraries(core_tests
    PRIVATE audiopipe_core Aoo::aoo Catch2::Catch2WithMain Threads::Threads
)

catch_discover_tests(core_tests)
