project(test)

file(GLOB SRC_LIST CONFIGURE_DEPENDS *.cc)

set(GALAY_REDIS_TARGETS_FILE "${CMAKE_BINARY_DIR}/galay-redis/galayRedisConfigTargets-build.cmake")

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/interface/verify_interface.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/interface/verify_interface.cmake
    @ONLY
)

add_test(
    NAME t24_interface_contract
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/interface/verify_interface.cmake
)
set_tests_properties(t24_interface_contract PROPERTIES LABELS "interface;unit")

set(GALAY_REDIS_UNIT_TESTS
    t3_protocol
    t8_batch
    t9_span
    t10_command
    t12_topology
    t14_size
    t15_surface
    t16_rediss
    t20_encoder
    t26_log
)

foreach(SRC ${SRC_LIST})
    get_filename_component(FILE_NAME ${SRC} NAME_WE)
    set(EXECUTABLE_NAME ${FILE_NAME})
    add_executable(${EXECUTABLE_NAME} ${SRC})
    set_property(TARGET ${EXECUTABLE_NAME} PROPERTY NO_SYSTEM_FROM_IMPORTED ON)
    target_link_libraries(${EXECUTABLE_NAME} PRIVATE galay-redis)
    target_include_directories(${EXECUTABLE_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/galay-redis)
    set_target_properties(${EXECUTABLE_NAME} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        INSTALL_RPATH $ORIGIN/../lib
    )
    add_test(NAME ${EXECUTABLE_NAME} COMMAND ${EXECUTABLE_NAME})
    if(EXECUTABLE_NAME STREQUAL "t13_cluster")
        set_tests_properties(${EXECUTABLE_NAME} PROPERTIES LABELS integration)
    elseif(EXECUTABLE_NAME IN_LIST GALAY_REDIS_UNIT_TESTS)
        set_tests_properties(${EXECUTABLE_NAME} PROPERTIES LABELS "unit;redis")
    else()
        set_tests_properties(${EXECUTABLE_NAME} PROPERTIES LABELS redis)
    endif()
endforeach()
