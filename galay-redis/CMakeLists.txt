find_package(galay-kernel REQUIRED)
find_package(galay-utils REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(spdlog REQUIRED)

include_directories(${OPENSSL_INCLUDE_DIR})

file(GLOB SOURCES "base/*.cc" "async/*.cc" "protocol/*.cc")

if(NOT TARGET galay-kernel::galay-kernel)
    message(FATAL_ERROR "galay-kernel::galay-kernel target not found. Please check galay-kernel installation and CMake config.")
endif()

if(NOT TARGET galay::galay-utils)
    message(FATAL_ERROR "galay::galay-utils target not found. Please check galay-utils installation and CMake config.")
endif()

add_library(${PROJECT_NAME} SHARED ${SOURCES})

# 链接 galay-kernel 和 galay-utils 库
target_link_libraries(${PROJECT_NAME}
    PUBLIC galay-kernel::galay-kernel
    PUBLIC galay::galay-utils
    PUBLIC OpenSSL::SSL    # 使用OpenSSL目标
    PUBLIC OpenSSL::Crypto # 使用OpenSSL目标
    PUBLIC pthread
)

target_include_directories(${PROJECT_NAME} PUBLIC ${OPENSSL_INCLUDE_DIR})
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR})

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