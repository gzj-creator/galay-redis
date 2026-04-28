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

add_redis_example(e1_basic include/e1_basic.cc)
add_redis_example(e2_pipeline include/e2_pipeline.cc)
add_redis_example(e3_pubsub include/e3_pubsub.cc)

if(TARGET galay-redis-modules)
    add_redis_import_example(e1_basic-import import/e1_basic.cc)
    add_redis_import_example(e2_pipeline-import import/e2_pipeline.cc)
    add_redis_import_example(e3_pubsub-import import/e3_pubsub.cc)
else()
    message(STATUS "Redis import examples are skipped (galay-redis-modules unavailable on this toolchain)")
endif()
