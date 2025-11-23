find_package(Galay REQUIRED)
find_package(OpenSSL REQUIRED)

include_directories(${OPENSSL_INCLUDE_DIR})

file(GLOB SOURCES "base/*.cc" "async/*.cc" "sync/*.cc" "protocol/*.cc")

if(NOT TARGET Galay::galay)
    message(FATAL_ERROR "Galay::Galay target not found. Please check Galay installation and CMake config.")
endif()

add_library(${PROJECT_NAME} SHARED ${SOURCES})

# 链接 Galay 库
target_link_libraries(${PROJECT_NAME}
    PUBLIC Galay::galay
    PUBLIC OpenSSL::SSL    # 使用OpenSSL目标
    PUBLIC OpenSSL::Crypto # 使用OpenSSL目标
    PUBLIC pthread
)

target_include_directories(${PROJECT_NAME} PUBLIC ${OPENSSL_INCLUDE_DIR})
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

set_target_properties(${PROJECT_NAME} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
)

# 安装编译完成的库文件
install(TARGETS ${PROJECT_NAME}
    EXPORT galay-redis-targets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)

# 安装 galay-http 目录下的头文件
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
    DESTINATION include/galay-redis
    FILES_MATCHING 
    PATTERN "*.h"
    PATTERN "*.hpp"
    PATTERN "*.inl"
    PATTERN "build" EXCLUDE
    PATTERN "CMakeFiles" EXCLUDE
    PATTERN "*.cc" EXCLUDE
    PATTERN "*.cmake" EXCLUDE
)