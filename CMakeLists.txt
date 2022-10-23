cmake_minimum_required(VERSION 3.21)

project(hello-protobuf)

set(CMAKE_CXX_STANDARD 20)

find_package(gRPC 1.50 CONFIG REQUIRED)
message(STATUS "Using gRPC ${gRPC_DIR} (Version ${gRPC_VERSION})")
include(cmake/ProtobufGenerateGrpc.cmake)

add_library(hw_grpc_proto OBJECT hello.proto)
target_include_directories(hw_grpc_proto PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(hw_grpc_proto PUBLIC gRPC::grpc++)
protobuf_generate_cpp_grpc(TARGET hw_grpc_proto)

add_executable(hello-server hello-server.cpp)
target_link_libraries(hello-server hw_grpc_proto fmt)
add_executable(hello-client hello-client.cpp)
target_link_libraries(hello-client hw_grpc_proto fmt)
