cmake_minimum_required(VERSION 3.20)
project(galay-redis)

message(STATUS "CXX_STANDARD: ${CMAKE_CXX_STANDARD}")

# 构建选项 / 平台选项
include(cmake/option.cmake)

# 配置 IO 后端
configure_io_backend()

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
