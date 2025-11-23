cmake_minimum_required(VERSION 3.20)
project(galay-redis)

message(STATUS "CXX_STANDARD: ${CMAKE_CXX_STANDARD}")

add_subdirectory(galay-redis)
add_subdirectory(test)