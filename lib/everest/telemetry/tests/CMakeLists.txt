set(TEST_TARGET_NAME telemetry_tests)
add_executable(${TEST_TARGET_NAME})

target_sources(${TEST_TARGET_NAME}
    PRIVATE
    set_publisher_test.cpp
    sink_test.cpp
)

target_include_directories(${TEST_TARGET_NAME}
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/tests/include
)

target_link_libraries(${TEST_TARGET_NAME}
    PRIVATE
    everest::telemetry
    GTest::gtest_main
)

add_test(${TEST_TARGET_NAME} ${TEST_TARGET_NAME})
ev_register_test_target(${TEST_TARGET_NAME})
