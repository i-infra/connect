cmake_minimum_required(VERSION 3.16)
project(nonolith-connect LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Git version
execute_process(
    COMMAND git rev-parse --short=16 HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_REV
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
execute_process(
    COMMAND git status --porcelain
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_STATUS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(GIT_STATUS MATCHES "^ [MADRC]")
    set(GIT_DIRTY "*")
endif()
set(GITVERSION "${GIT_REV}${GIT_DIRTY}")

# Dependencies
find_package(Boost REQUIRED COMPONENTS regex thread)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)

# libjson (in-tree static library)
file(GLOB LIBJSON_SOURCES libjson/_internal/Source/*.cpp)
add_library(json STATIC ${LIBJSON_SOURCES})
target_include_directories(json PRIVATE libjson/_internal/Source)

# Main executable sources
set(SOURCES
    device.cpp
    event.cpp
    rest_service.cpp
    server.cpp
    session.cpp
    url.cpp
    usb.cpp
    version.cpp
    websocket_service.cpp
    streaming_device/output_source.cpp
    streaming_device/rest_api.cpp
    streaming_device/stream_listener.cpp
    streaming_device/streaming_device.cpp
    streaming_device/ws_api.cpp
    cee/cee.cpp
    m1k/m1k.cpp
    bootloader/bootloader.cpp
)

add_executable(nonolith-connect ${SOURCES})

target_compile_definitions(nonolith-connect PRIVATE
    $<$<BOOL:${GITVERSION}>:GITVERSION=${GITVERSION}>
)

target_include_directories(nonolith-connect PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${LIBUSB_INCLUDE_DIRS}
)

target_link_libraries(nonolith-connect PRIVATE
    json
    Boost::regex
    Boost::thread
    ${LIBUSB_LIBRARIES}
)

target_link_directories(nonolith-connect PRIVATE
    ${LIBUSB_LIBRARY_DIRS}
)

# Tests: link the protocol/handler stack (no sockets, no USB) against the
# doctest suite in tests/. server.cpp, session.cpp, usb.cpp and the hardware
# device classes are excluded; tests/main.cpp provides the shared globals.
enable_testing()

set(TEST_CORE_SOURCES
    device.cpp
    event.cpp
    rest_service.cpp
    url.cpp
    version.cpp
    websocket_service.cpp
    streaming_device/output_source.cpp
    streaming_device/rest_api.cpp
    streaming_device/stream_listener.cpp
    streaming_device/streaming_device.cpp
    streaming_device/ws_api.cpp
)

file(GLOB TEST_SOURCES tests/main.cpp tests/test_*.cpp)

add_executable(connect-tests ${TEST_CORE_SOURCES} ${TEST_SOURCES})

target_include_directories(connect-tests PRIVATE ${CMAKE_SOURCE_DIR})

target_link_libraries(connect-tests PRIVATE
    json
    Boost::regex
    Boost::thread
)

add_test(NAME unit COMMAND connect-tests)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
    add_test(NAME e2e
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/e2e.py
                $<TARGET_FILE:nonolith-connect>
    )
    set_tests_properties(e2e PROPERTIES TIMEOUT 60)
endif()

# Platform-specific
if(APPLE)
    target_link_libraries(nonolith-connect PRIVATE
        "-framework CoreFoundation"
        "-framework IOKit"
        objc
    )
    target_compile_options(nonolith-connect PRIVATE -fstack-protector-all)
    target_link_options(nonolith-connect PRIVATE -fstack-protector-all)
elseif(UNIX)
    target_link_libraries(nonolith-connect PRIVATE
        udev pthread rt
    )
    target_compile_options(nonolith-connect PRIVATE -fstack-protector-all)
    target_compile_definitions(nonolith-connect PRIVATE _FORTIFY_SOURCE=2)
    target_link_options(nonolith-connect PRIVATE -fstack-protector-all)
endif()
