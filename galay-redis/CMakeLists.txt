find_package(galay-kernel 3.4.4 CONFIG REQUIRED)
find_package(galay-utils 1.0.3 CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(spdlog REQUIRED)
include(CMakePackageConfigHelpers)

if(GALAY_REDIS_ENABLE_SSL)
    find_package(galay-ssl 1.2.2 CONFIG REQUIRED)

    if(NOT TARGET galay-ssl::galay-ssl)
        message(FATAL_ERROR "galay-ssl::galay-ssl target not found. Please install galay-ssl or disable GALAY_REDIS_ENABLE_SSL.")
    endif()
endif()

option(GALAY_REDIS_INSTALL_MODULE_INTERFACE "Install C++ module interface files (*.cppm)" ON)
option(GALAY_REDIS_ENABLE_IMPORT_COMPILATION "Enable C++ import/module compilation path" ON)

set(GALAY_REDIS_IMPORT_BUILD_AVAILABLE OFF)
set(GALAY_REDIS_IMPORT_COMPILATION_ENABLED OFF)
set(GALAY_REDIS_IMPORT_COMPILE_OPTIONS "")
set(GALAY_REDIS_IMPORT_UNAVAILABLE_REASON "")

if(CMAKE_VERSION VERSION_LESS 3.28)
    set(GALAY_REDIS_IMPORT_UNAVAILABLE_REASON "CMake < 3.28")
else()
    if(NOT CMAKE_GENERATOR MATCHES "Ninja|Visual Studio")
        set(GALAY_REDIS_IMPORT_UNAVAILABLE_REASON "unsupported generator; require Ninja or Visual Studio")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
            set(GALAY_REDIS_IMPORT_UNAVAILABLE_REASON "current stable GNU path is Linux only")
        elseif(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
            set(GALAY_REDIS_IMPORT_UNAVAILABLE_REASON "GCC < 14")
        else()
            set(GALAY_REDIS_IMPORT_BUILD_AVAILABLE ON)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        find_program(GALAY_REDIS_CLANG_SCAN_DEPS NAMES clang-scan-deps)
        if(NOT GALAY_REDIS_CLANG_SCAN_DEPS)
            get_filename_component(_GALAY_REDIS_CLANG_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
            if(EXISTS "${_GALAY_REDIS_CLANG_BIN_DIR}/clang-scan-deps")
                set(GALAY_REDIS_CLANG_SCAN_DEPS "${_GALAY_REDIS_CLANG_BIN_DIR}/clang-scan-deps")
            else()
                find_program(GALAY_REDIS_CLANG_SCAN_DEPS NAMES clang-scan-deps HINTS "${_GALAY_REDIS_CLANG_BIN_DIR}")
            endif()
        endif()
        if(NOT GALAY_REDIS_CLANG_SCAN_DEPS)
            set(GALAY_REDIS_IMPORT_UNAVAILABLE_REASON "clang-scan-deps not found")
        elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
            set(GALAY_REDIS_IMPORT_UNAVAILABLE_REASON "AppleClang is not supported for module dependency scanning in this project")
        else()
            set(GALAY_REDIS_IMPORT_BUILD_AVAILABLE ON)
            list(APPEND GALAY_REDIS_IMPORT_COMPILE_OPTIONS -Xclang -fskip-odr-check-in-gmf)
            if(APPLE)
                if(CMAKE_OSX_SYSROOT)
                    list(APPEND GALAY_REDIS_IMPORT_COMPILE_OPTIONS -isysroot "${CMAKE_OSX_SYSROOT}")
                else()
                    execute_process(
                        COMMAND xcrun --show-sdk-path
                        OUTPUT_VARIABLE GALAY_REDIS_MACOS_SDK_PATH
                        OUTPUT_STRIP_TRAILING_WHITESPACE
                        ERROR_QUIET
                    )
                    if(GALAY_REDIS_MACOS_SDK_PATH)
                        list(APPEND GALAY_REDIS_IMPORT_COMPILE_OPTIONS -isysroot "${GALAY_REDIS_MACOS_SDK_PATH}")
                    endif()
                endif()
            endif()
        endif()
    else()
        set(GALAY_REDIS_IMPORT_UNAVAILABLE_REASON
            "unsupported compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
    endif()
endif()

if(GALAY_REDIS_ENABLE_IMPORT_COMPILATION AND GALAY_REDIS_IMPORT_BUILD_AVAILABLE)
    set(GALAY_REDIS_IMPORT_COMPILATION_ENABLED ON)
elseif(GALAY_REDIS_ENABLE_IMPORT_COMPILATION AND NOT GALAY_REDIS_IMPORT_BUILD_AVAILABLE)
    message(STATUS "GALAY_REDIS_ENABLE_IMPORT_COMPILATION=ON but toolchain is unsupported. "
                   "Import compilation is disabled. "
                   "Requirement: CMake >= 3.28, Ninja/Visual Studio generator, Linux + GCC >= 14 or Clang + clang-scan-deps. "
                   "Current: ${CMAKE_SYSTEM_NAME}, ${CMAKE_GENERATOR}, ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}. "
                   "Reason: ${GALAY_REDIS_IMPORT_UNAVAILABLE_REASON}.")
endif()

file(GLOB SOURCES "base/*.cc" "async/*.cc" "protocol/*.cc")

if(NOT TARGET galay-kernel::galay-kernel)
    message(FATAL_ERROR "galay-kernel::galay-kernel target not found. Please check galay-kernel installation and CMake config.")
endif()

if(NOT TARGET galay::galay-utils)
    message(FATAL_ERROR "galay::galay-utils target not found. Please check galay-utils installation and CMake config.")
endif()

add_library(${PROJECT_NAME} SHARED ${SOURCES})
set_property(TARGET ${PROJECT_NAME} PROPERTY NO_SYSTEM_FROM_IMPORTED ON)

# 链接 galay-kernel 和 galay-utils 库
target_link_libraries(${PROJECT_NAME}
    PUBLIC
        galay-kernel::galay-kernel
        spdlog::spdlog
    PRIVATE
        galay::galay-utils
        OpenSSL::SSL
        OpenSSL::Crypto
)

if(GALAY_REDIS_ENABLE_SSL)
    target_link_libraries(${PROJECT_NAME}
        PUBLIC galay-ssl::galay-ssl
    )
    target_compile_definitions(${PROJECT_NAME}
        PUBLIC GALAY_REDIS_SSL_ENABLED
    )
endif()

target_include_directories(${PROJECT_NAME}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>
)

set_target_properties(${PROJECT_NAME} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
)

if(GALAY_REDIS_IMPORT_COMPILATION_ENABLED AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
    add_library(${PROJECT_NAME}-modules STATIC)
    target_link_libraries(${PROJECT_NAME}-modules PUBLIC ${PROJECT_NAME})
    set_target_properties(${PROJECT_NAME}-modules PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
    )
    target_sources(${PROJECT_NAME}-modules
        PUBLIC
            FILE_SET CXX_MODULES
            FILES ${CMAKE_CURRENT_SOURCE_DIR}/module/galay.redis.cppm
    )
    if(GALAY_REDIS_IMPORT_COMPILE_OPTIONS)
        target_compile_options(${PROJECT_NAME}-modules PUBLIC ${GALAY_REDIS_IMPORT_COMPILE_OPTIONS})
    endif()
endif()

# 安装编译完成的库文件
install(TARGETS ${PROJECT_NAME}
    EXPORT galay-redis-targets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)

if(TARGET ${PROJECT_NAME}-modules AND GALAY_REDIS_INSTALL_MODULE_INTERFACE)
    install(TARGETS ${PROJECT_NAME}-modules
        EXPORT galay-redis-targets
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        RUNTIME DESTINATION bin
        FILE_SET CXX_MODULES DESTINATION include/galay-redis/module
    )
endif()

export(
    EXPORT galay-redis-targets
    FILE "${CMAKE_CURRENT_BINARY_DIR}/galay-redis-targets-build.cmake"
    NAMESPACE galay-redis::
)

# 安装公开头文件，显式排除遗留 sync/ 表面
foreach(GALAY_REDIS_PUBLIC_SUBDIR async base protocol module)
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/${GALAY_REDIS_PUBLIC_SUBDIR}/
        DESTINATION include/galay-redis/${GALAY_REDIS_PUBLIC_SUBDIR}
        FILES_MATCHING
        PATTERN "*.h"
        PATTERN "*.hpp"
        PATTERN "*.inl"
    )
endforeach()

if(GALAY_REDIS_INSTALL_MODULE_INTERFACE AND NOT GALAY_REDIS_IMPORT_COMPILATION_ENABLED)
    file(GLOB GALAY_REDIS_MODULE_INTERFACE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/module/*.cppm")
    if(GALAY_REDIS_MODULE_INTERFACE_FILES)
        install(FILES ${GALAY_REDIS_MODULE_INTERFACE_FILES}
            DESTINATION include/galay-redis/module
        )
    endif()
endif()

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/galay-redis-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
    "${CMAKE_SOURCE_DIR}/galay-redis-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/galay-redis-config.cmake"
    INSTALL_DESTINATION lib/cmake/galay-redis
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/galay-redis-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/galay-redis-config-version.cmake"
    DESTINATION lib/cmake/galay-redis
)

if(CMAKE_CONFIGURATION_TYPES)
    install(EXPORT galay-redis-targets
        FILE galay-redis-targets.cmake
        NAMESPACE galay-redis::
        DESTINATION lib/cmake/galay-redis
    )
else()
    install(EXPORT galay-redis-targets
        FILE galay-redis-targets.cmake
        NAMESPACE galay-redis::
        DESTINATION lib/cmake/galay-redis
        CONFIGURATIONS ${CMAKE_BUILD_TYPE}
    )
endif()
