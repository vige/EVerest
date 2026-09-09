# Telemetry consumer helper

add_library(telemetry STATIC)
add_library(everest::telemetry ALIAS telemetry)
ev_register_library_target(telemetry)

target_sources(telemetry
    PRIVATE
        src/sink.cpp
)

target_include_directories(telemetry
    PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    "$<TARGET_PROPERTY:generate_cpp_files,EVEREST_GENERATED_INCLUDE_DIR>"
)

add_dependencies(telemetry generate_cpp_files)

target_link_libraries(telemetry
    PUBLIC
        everest::framework
)

if(EVEREST_CORE_BUILD_TESTING)
    add_subdirectory(tests)
endif()
