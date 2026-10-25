# Unit tests for nanobot-manager. Uses a self-contained framework (nm_test.h)
# and CTest. Each module is compiled into its own console executable.

add_library(nm_test INTERFACE)
target_include_directories(nm_test INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# Define a manager unit-test executable from <name>.c (or <name>.cpp when the
# .cpp variant exists) plus any extra sources.
# Console subsystem (WIN32_EXECUTABLE FALSE) so CTest captures output.
# Link extra libs / add include dirs afterwards with the normal commands.
function(nm_add_test name)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${name}.cpp)
        set(_nm_test_src ${name}.cpp)
    else()
        set(_nm_test_src ${name}.c)
    endif()
    add_executable(${name} ${_nm_test_src} ${ARGN})
    target_link_libraries(${name} PRIVATE nm_test)
    target_compile_definitions(${name} PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
    )
    target_compile_options(${name} PRIVATE /utf-8 /EHsc)
    set_target_properties(${name} PROPERTIES WIN32_EXECUTABLE FALSE)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# config: pure JSON logic. config.c is compiled directly here (avoids linking
# main.cpp's WinMain). Needs the src include dir + cJSON headers + the cjson lib.
nm_add_test(test_config ${CMAKE_SOURCE_DIR}/src/config.c)
target_include_directories(test_config PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${cjson_SOURCE_DIR}
)
target_link_libraries(test_config PRIVATE cjson shell32 ole32 advapi32)

nm_add_test(test_credential ${CMAKE_SOURCE_DIR}/src/credential.c)
target_include_directories(test_credential PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_credential PRIVATE advapi32)

nm_add_test(test_process
    ${CMAKE_SOURCE_DIR}/src/process.c
    ${CMAKE_SOURCE_DIR}/src/logger.c
)
target_include_directories(test_process PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${cjson_SOURCE_DIR}
)
target_link_libraries(test_process PRIVATE cjson winhttp shell32 ole32 advapi32)

# autostart: real HKCU\...\Run round-trip (saves & restores the original value).
# advapi32 provides the registry API used both in the test and in autostart.c.
nm_add_test(test_autostart ${CMAKE_SOURCE_DIR}/src/autostart.c)
target_include_directories(test_autostart PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_autostart PRIVATE advapi32)

nm_add_test(test_chat_history ${CMAKE_SOURCE_DIR}/src/chat_history.c)
target_include_directories(test_chat_history PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${cjson_SOURCE_DIR}
)
target_link_libraries(test_chat_history PRIVATE cjson)

nm_add_test(test_chat_protocol
    ${CMAKE_SOURCE_DIR}/src/chat_protocol.cpp
    ${CMAKE_SOURCE_DIR}/src/chat_history.c
)
target_include_directories(test_chat_protocol PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${cjson_SOURCE_DIR}
)
target_link_libraries(test_chat_protocol PRIVATE cjson)

# chat_draft: bounded streaming buffer (pure C++ logic, header-only).
nm_add_test(test_chat_draft)
target_include_directories(test_chat_draft PRIVATE ${CMAKE_SOURCE_DIR}/src)

# logger: file pipeline (level filter, format, rotation) against a temp dir.
nm_add_test(test_logger ${CMAKE_SOURCE_DIR}/src/logger.c)
target_include_directories(test_logger PRIVATE ${CMAKE_SOURCE_DIR}/src)

# i18n: pure string-table assembly + zh/en key parity (no platform deps).
nm_add_test(test_i18n ${CMAKE_SOURCE_DIR}/src/i18n.c)
target_include_directories(test_i18n PRIVATE ${CMAKE_SOURCE_DIR}/src)
