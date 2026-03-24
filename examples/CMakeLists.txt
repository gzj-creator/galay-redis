set(GALAY_REDIS_EXAMPLE_KERNEL_INCLUDE_DIRS "")
if(TARGET galay-kernel::galay-kernel)
    get_target_property(GALAY_REDIS_EXAMPLE_KERNEL_INCLUDE_DIRS galay-kernel::galay-kernel INTERFACE_INCLUDE_DIRECTORIES)
endif()

function(add_redis_example target_name source_file)
    add_executable(${target_name} ${source_file})
    set_property(TARGET ${target_name} PROPERTY NO_SYSTEM_FROM_IMPORTED ON)
    target_link_libraries(${target_name} PRIVATE galay-redis)
    target_include_directories(${target_name} PRIVATE
        ${CMAKE_SOURCE_DIR}
    )
    if(GALAY_REDIS_EXAMPLE_KERNEL_INCLUDE_DIRS)
        foreach(GALAY_REDIS_EXAMPLE_KERNEL_INCLUDE_DIR ${GALAY_REDIS_EXAMPLE_KERNEL_INCLUDE_DIRS})
            target_compile_options(${target_name} BEFORE PRIVATE "-I${GALAY_REDIS_EXAMPLE_KERNEL_INCLUDE_DIR}")
        endforeach()
    endif()
    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        INSTALL_RPATH $ORIGIN/../lib
    )
endfunction()

if(BUILD_EXAMPLES)
    add_redis_example(E1-async_basic_demo include/E1-async_basic_demo.cc)
    add_redis_example(E2-pipeline_demo include/E2-pipeline_demo.cc)
    add_redis_example(E3-topology_pubsub_demo include/E3-topology_pubsub_demo.cc)
endif()
