project(test)
file(GLOB SRC_LIST *.cc)

# 在find_package之前设置环境变量
set(OPENSSL_ROOT_DIR "/opt/homebrew")
set(ENV{OPENSSL_ROOT_DIR} "/opt/homebrew")

find_package(galay-kernel REQUIRED)
find_package(galay-utils REQUIRED)
find_package(OpenSSL REQUIRED)

set(GALAY_REDIS_TEST_KERNEL_INCLUDE_DIRS "")
if(TARGET galay-kernel::galay-kernel)
    get_target_property(GALAY_REDIS_TEST_KERNEL_INCLUDE_DIRS galay-kernel::galay-kernel INTERFACE_INCLUDE_DIRECTORIES)
endif()

# 确保包含目录正确设置
include_directories(${OPENSSL_INCLUDE_DIR})
include_directories(${CMAKE_SOURCE_DIR}/galay-redis)

foreach(SRC ${SRC_LIST})
    get_filename_component(FILE_NAME ${SRC} NAME_WE)
    set(EXECUTABLE_NAME ${FILE_NAME})
    add_executable(${EXECUTABLE_NAME} ${SRC})
    set_property(TARGET ${EXECUTABLE_NAME} PROPERTY NO_SYSTEM_FROM_IMPORTED ON)
    target_link_libraries(${EXECUTABLE_NAME} 
        galay-redis
        OpenSSL::SSL
        OpenSSL::Crypto
    )
    # 为每个目标明确设置包含目录
    target_include_directories(${EXECUTABLE_NAME} PRIVATE 
        ${OPENSSL_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}/galay-redis
    )
    if(GALAY_REDIS_TEST_KERNEL_INCLUDE_DIRS)
        foreach(GALAY_REDIS_TEST_KERNEL_INCLUDE_DIR ${GALAY_REDIS_TEST_KERNEL_INCLUDE_DIRS})
            target_compile_options(${EXECUTABLE_NAME} BEFORE PRIVATE "-I${GALAY_REDIS_TEST_KERNEL_INCLUDE_DIR}")
        endforeach()
    endif()
    set_target_properties(${EXECUTABLE_NAME} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        INSTALL_RPATH $ORIGIN/../lib
    )
    add_test(NAME ${EXECUTABLE_NAME} COMMAND ${EXECUTABLE_NAME})
    list(APPEND INSTALL_BIN ${EXECUTABLE_NAME})
endforeach()
