cmake_minimum_required(VERSION 3.20)
project(galay-redis)

message(STATUS "CXX_STANDARD: ${CMAKE_CXX_STANDARD}")

option(BUILD_TESTS "Build test programs" ON)
option(BUILD_BENCHMARKS "Build benchmark programs" ON)
option(BUILD_EXAMPLES "Build example programs" ON)

add_subdirectory(galay-redis)

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

if(BUILD_BENCHMARKS)
    add_subdirectory(benchmark)
endif()

if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
