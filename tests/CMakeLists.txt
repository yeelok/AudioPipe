include(Catch)

add_executable(core_tests
    test_sanity.cpp
    test_config.cpp
)

target_link_libraries(core_tests
    PRIVATE audiopipe_core Catch2::Catch2WithMain
)

catch_discover_tests(core_tests)
