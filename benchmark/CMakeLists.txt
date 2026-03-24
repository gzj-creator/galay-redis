project(benchmark)

include_directories(${CMAKE_SOURCE_DIR}/galay-redis)
set(GALAY_REDIS_BENCH_KERNEL_INCLUDE_DIRS "")
if(TARGET galay-kernel::galay-kernel)
    get_target_property(GALAY_REDIS_BENCH_KERNEL_INCLUDE_DIRS galay-kernel::galay-kernel INTERFACE_INCLUDE_DIRECTORIES)
endif()

file(GLOB BENCHMARK_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")

foreach(BENCHMARK_SOURCE ${BENCHMARK_SOURCES})
    get_filename_component(BENCHMARK_FULL_NAME ${BENCHMARK_SOURCE} NAME)
    string(REPLACE ".cc" "" BENCHMARK_NAME ${BENCHMARK_FULL_NAME})

    add_executable(${BENCHMARK_NAME} ${BENCHMARK_SOURCE})
    set_property(TARGET ${BENCHMARK_NAME} PROPERTY NO_SYSTEM_FROM_IMPORTED ON)
    target_link_libraries(${BENCHMARK_NAME} galay-redis)
    target_include_directories(${BENCHMARK_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}
    )
    if(GALAY_REDIS_BENCH_KERNEL_INCLUDE_DIRS)
        foreach(GALAY_REDIS_BENCH_KERNEL_INCLUDE_DIR ${GALAY_REDIS_BENCH_KERNEL_INCLUDE_DIRS})
            target_compile_options(${BENCHMARK_NAME} BEFORE PRIVATE "-I${GALAY_REDIS_BENCH_KERNEL_INCLUDE_DIR}")
        endforeach()
    endif()
    set_target_properties(${BENCHMARK_NAME} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        INSTALL_RPATH $ORIGIN/../lib
    )
endforeach()
