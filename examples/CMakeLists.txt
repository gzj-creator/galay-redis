function(add_redis_example target_name source_file)
    add_executable(${target_name} ${source_file})
    set_property(TARGET ${target_name} PROPERTY NO_SYSTEM_FROM_IMPORTED ON)
    target_link_libraries(${target_name} PRIVATE galay-redis)
    target_include_directories(${target_name} PRIVATE
        ${CMAKE_SOURCE_DIR}
    )
    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        INSTALL_RPATH $ORIGIN/../lib
    )
endfunction()

function(add_redis_import_example target_name source_file)
    add_executable(${target_name} ${source_file})
    set_property(TARGET ${target_name} PROPERTY NO_SYSTEM_FROM_IMPORTED ON)
    target_link_libraries(${target_name} PRIVATE galay-redis-modules)
    target_include_directories(${target_name} PRIVATE
        ${CMAKE_SOURCE_DIR}
    )
    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        CXX_SCAN_FOR_MODULES ON
        INSTALL_RPATH $ORIGIN/../lib
    )
endfunction()

add_redis_example(E1-async_basic_demo include/E1-async_basic_demo.cc)
add_redis_example(E2-pipeline_demo include/E2-pipeline_demo.cc)
add_redis_example(E3-topology_pubsub_demo include/E3-topology_pubsub_demo.cc)

if(TARGET galay-redis-modules)
    add_redis_import_example(E1-async_basic_demo-import import/E1-async_basic_demo.cc)
    add_redis_import_example(E2-pipeline_demo-import import/E2-pipeline_demo.cc)
    add_redis_import_example(E3-topology_pubsub_demo-import import/E3-topology_pubsub_demo.cc)
else()
    message(STATUS "Redis import examples are skipped (galay-redis-modules unavailable on this toolchain)")
endif()
