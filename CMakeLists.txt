cmake_minimum_required(VERSION 3.21)

project(asio-grpc-callback)

set(CMAKE_CXX_STANDARD 20)

option(USE_ASIO_CORO "Use experimental::coro in server stream (if C++20 is supported)" ON)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_SOURCE_DIR}/cmake)
include(CPM)
include(ProtobufGenerateGrpc)

find_package(gRPC 1.50 CONFIG REQUIRED)
message(STATUS "Using gRPC ${gRPC_DIR} (Version ${gRPC_VERSION})")

CPMAddPackage(
        NAME asio
        URL https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-24-0.tar.gz
        VERSION 1.24.0
)
if (asio_ADDED)
    add_library(asio INTERFACE)
    target_include_directories(asio SYSTEM INTERFACE ${asio_SOURCE_DIR}/asio/include)
    target_compile_definitions(asio INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)
    target_link_libraries(asio INTERFACE pthread)
endif ()

add_subdirectory(src)
