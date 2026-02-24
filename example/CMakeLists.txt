function(add_redis_example target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay-redis)
    target_include_directories(${target_name} PRIVATE
        ${CMAKE_SOURCE_DIR}
        /usr/local/include
    )
    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        INSTALL_RPATH $ORIGIN/../lib
    )
endfunction()

if(BUILD_EXAMPLES)
    add_redis_example(E1-AsyncBasicDemo include/E1-AsyncBasicDemo.cpp)
    add_redis_example(E2-PipelineDemo include/E2-PipelineDemo.cpp)
    add_redis_example(E3-TopologyPubSubDemo include/E3-TopologyPubSubDemo.cpp)
endif()
