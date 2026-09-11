# OCPP module common
#
# Support code shared by the OCPP modules (conversions between EVerest and
# OCPP types, transaction handling, error handling/mapping and the EVerest
# device model storage).

add_library(ocpp_module_common STATIC)
add_library(everest::ocpp_module_common ALIAS ocpp_module_common)
ev_register_library_target(ocpp_module_common)

target_sources(ocpp_module_common
    PRIVATE
        src/conversions.cpp
        src/error_handling.cpp
        src/transaction_handler.cpp
        src/v16/conversions.cpp
        src/device_model/composed_device_model_storage.cpp
        src/device_model/definitions.cpp
        src/device_model/everest_device_model_storage.cpp
)

# The OTLP receiver: an HTTP endpoint a producer posts metrics to, the decoder behind it, and the
# device model source it feeds. All of it is optional, so all of it is behind the flag -- nothing
# here is compiled, and no dependency is fetched, in a build that does not ask for telemetry. The
# option itself lives in the top-level CMakeLists, because dependencies.yaml reads it.
if(EVEREST_ENABLE_OTLP_TELEMETRY)
    if(DISABLE_EDM)
        find_package(libwebsockets REQUIRED)
        find_package(opentelemetry-cpp REQUIRED)
    endif()
    target_sources(ocpp_module_common
        PRIVATE
            src/otlp/http_server.cpp
            src/otlp/metrics_decode.cpp
            src/device_model/telemetry_device_model_storage.cpp
            src/device_model/telemetry_mapping.cpp
    )
    target_compile_definitions(ocpp_module_common PUBLIC EVEREST_ENABLE_OTLP_TELEMETRY)
    # opentelemetry_proto is the generated OTLP code opentelemetry-cpp compiles for its own
    # exporter, and it carries protobuf with it. The producers of a station encode with these
    # definitions, so the receiver decodes with the same ones rather than a transcription of them.
    target_link_libraries(ocpp_module_common
        PRIVATE
            websockets_shared
            opentelemetry-cpp::proto
    )
endif()

target_include_directories(ocpp_module_common
    PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    "$<BUILD_INTERFACE:$<TARGET_PROPERTY:generate_cpp_files,EVEREST_GENERATED_INCLUDE_DIR>>"
)

target_compile_options(ocpp_module_common
    PRIVATE
        -Wimplicit-fallthrough
        -Werror=switch-enum
)

add_dependencies(ocpp_module_common generate_cpp_files)

target_link_libraries(ocpp_module_common
    PUBLIC
        everest::ocpp
        everest::framework
    PRIVATE
        everest::ocpp_conversions
        everest::yaml
)

if (DISABLE_EDM)
    install(
        TARGETS ocpp_module_common
        EXPORT everest-core-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/everest
    )
    evc_setup_package_no_export(
        NAME everest-ocpp-module-common
        LIBRARY_NAME ocpp_module_common
        NAMESPACE everest
        TYPE STATIC
    )
endif()

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/everest
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp"
)

if(EVEREST_CORE_BUILD_TESTING)
    add_subdirectory(tests)
endif()
