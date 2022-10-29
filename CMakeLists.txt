cmake_minimum_required(VERSION 3.21)

project(hello-protobuf)

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

add_library(hw_grpc_proto OBJECT hello.proto)
target_include_directories(hw_grpc_proto PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(hw_grpc_proto PUBLIC gRPC::grpc++)
protobuf_generate_cpp_grpc(TARGET hw_grpc_proto)

add_library(common common.cpp)
target_link_libraries(common INTERFACE fmt asio)

foreach(variant sync async17 async20)
    add_executable(hello-server-${variant} hello-server-${variant}.cpp)
    target_link_libraries(hello-server-${variant} hw_grpc_proto common)
    if (USE_ASIO_CORO)
        target_compile_definitions(hello-server-${variant} PRIVATE USE_ASIO_CORO)
    endif()
    add_executable(hello-client-${variant} hello-client-${variant}.cpp)
    target_link_libraries(hello-client-${variant} hw_grpc_proto common)
endforeach()

if ("${CMAKE_CXX_COMPILE_FEATURES}" MATCHES cxx_std_20)
    add_executable(test-asio test-asio.cpp)
    target_link_libraries(test-asio common)
    target_compile_features(test-asio PRIVATE cxx_std_20)
endif()
