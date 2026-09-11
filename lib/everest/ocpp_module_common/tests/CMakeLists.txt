if(DISABLE_EDM)
    find_package(nlohmann_json REQUIRED)
endif()

set(TEST_TARGET_NAME ocpp_module_common_tests)
add_executable(${TEST_TARGET_NAME})

target_sources(${TEST_TARGET_NAME}
    PRIVATE
    composed_device_model_storage_test.cpp
    data_transfer_conversions_test.cpp
    error_handling_tests.cpp
    everest_device_model_storage_test.cpp
    grid_support_conversions_test.cpp
    network_conversions_test.cpp
    transaction_handler_tests.cpp
)

# The telemetry tests follow their subject: without the flag there is no decoder and no telemetry
# device model source to test.
if(EVEREST_ENABLE_OTLP_TELEMETRY)
    target_sources(${TEST_TARGET_NAME}
        PRIVATE
        otlp_metrics_decode_test.cpp
        telemetry_device_model_storage_test.cpp
    )
endif()

target_compile_definitions(${TEST_TARGET_NAME}
    PRIVATE
    TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    DEVICE_MODEL_MIGRATIONS_DIR="${PROJECT_SOURCE_DIR}/lib/everest/ocpp/config/common/device_model_migrations"
)

target_link_libraries(${TEST_TARGET_NAME}
    PRIVATE
    everest::ocpp_module_common
    nlohmann_json::nlohmann_json
    GTest::gtest_main
)

add_test(${TEST_TARGET_NAME} ${TEST_TARGET_NAME})
ev_register_test_target(${TEST_TARGET_NAME})
