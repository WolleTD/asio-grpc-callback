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

add_executable(hello-server hello-server.cpp)
target_link_libraries(hello-server hw_grpc_proto fmt asio)
if (USE_ASIO_CORO)
    target_compile_definitions(hello-server PRIVATE USE_ASIO_CORO)
endif()
add_executable(hello-client hello-client.cpp)
target_link_libraries(hello-client hw_grpc_proto fmt asio)

if ("${CMAKE_CXX_COMPILE_FEATURES}" MATCHES cxx_std_20)
    add_executable(test-asio test-asio.cpp)
    target_link_libraries(test-asio fmt asio)
    target_compile_features(test-asio PRIVATE cxx_std_20)
endif()
