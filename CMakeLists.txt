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
