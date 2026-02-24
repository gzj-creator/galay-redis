project(benchmark)

include_directories(${CMAKE_SOURCE_DIR}/galay-redis)
include_directories(/usr/local/include)

file(GLOB BENCHMARK_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")

foreach(BENCHMARK_SOURCE ${BENCHMARK_SOURCES})
    get_filename_component(BENCHMARK_FULL_NAME ${BENCHMARK_SOURCE} NAME)
    string(REPLACE ".cc" "" BENCHMARK_NAME ${BENCHMARK_FULL_NAME})

    add_executable(${BENCHMARK_NAME} ${BENCHMARK_SOURCE})
    target_link_libraries(${BENCHMARK_NAME} galay-redis)
    target_include_directories(${BENCHMARK_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}
        /usr/local/include
    )
    set_target_properties(${BENCHMARK_NAME} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        INSTALL_RPATH $ORIGIN/../lib
    )
endforeach()
