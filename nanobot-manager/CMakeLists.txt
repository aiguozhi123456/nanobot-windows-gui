cmake_minimum_required(VERSION 3.20)
project(nanobot-manager C CXX)

set(NANOBOT_MANAGER_VERSION "1.5.0" CACHE STRING "Project version" FORCE)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/version.h.in
    ${CMAKE_BINARY_DIR}/generated/version.h
    @ONLY
)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/installer/version.nsh.in
    ${CMAKE_BINARY_DIR}/generated/version.nsh
    @ONLY
)

set(UI_CORE_BUILD_STATIC_ARCHIVE OFF CACHE BOOL "" FORCE)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../core-ui ${CMAKE_BINARY_DIR}/core-ui-build)

include(FetchContent)
FetchContent_Declare(
    cjson
    GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
    GIT_TAG        v1.7.18
    GIT_SHALLOW    TRUE
)
set(CJSON_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_CJSON_TEST OFF CACHE BOOL "" FORCE)
set(CJSON_OVERRIDE_BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cjson)

add_executable(nanobot-manager WIN32
    src/main.cpp
    src/process.c
    src/autostart.c
    src/config.c
    src/resources.rc
)

target_include_directories(nanobot-manager PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
    ${cjson_SOURCE_DIR}
)

target_link_libraries(nanobot-manager PRIVATE
    core-ui
    cjson
    winhttp
    advapi32
    shell32
    shlwapi
    userenv
    comctl32
)

target_compile_options(nanobot-manager PRIVATE /utf-8 /EHsc)

target_compile_definitions(nanobot-manager PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

add_custom_command(TARGET nanobot-manager POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/ui
        $<TARGET_FILE_DIR:nanobot-manager>/ui
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:core-ui>
        $<TARGET_FILE_DIR:nanobot-manager>
)
