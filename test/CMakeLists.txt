project(test)
file(GLOB SRC_LIST *.cc)

# 在find_package之前设置环境变量
set(OPENSSL_ROOT_DIR "/opt/homebrew")
set(ENV{OPENSSL_ROOT_DIR} "/opt/homebrew")

find_package(galay-kernel REQUIRED)
find_package(galay-utils REQUIRED)
find_package(OpenSSL REQUIRED)

# 确保包含目录正确设置
include_directories(${OPENSSL_INCLUDE_DIR})
include_directories(${CMAKE_SOURCE_DIR}/galay-redis)
# 添加/usr/local/include以确保galay-kernel库能访问其头文件
include_directories(/usr/local/include)

foreach(SRC ${SRC_LIST})
    get_filename_component(FILE_NAME ${SRC} NAME_WE)
    set(EXECUTABLE_NAME ${FILE_NAME})
    add_executable(${EXECUTABLE_NAME} ${SRC})
    target_link_libraries(${EXECUTABLE_NAME} 
        galay-redis
        OpenSSL::SSL
        OpenSSL::Crypto
        pthread
    )
    # 为每个目标明确设置包含目录
    target_include_directories(${EXECUTABLE_NAME} PRIVATE 
        ${OPENSSL_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}/galay-redis
        /usr/local/include
    )
    set_target_properties(${EXECUTABLE_NAME} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        INSTALL_RPATH $ORIGIN/../lib
    )
    list(APPEND INSTALL_BIN ${EXECUTABLE_NAME})
endforeach()