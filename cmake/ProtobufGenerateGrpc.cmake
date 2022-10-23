function(protobuf_generate_cpp_grpc)
    set(GRPC_PLUGIN "protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>")

    protobuf_generate(${ARGN} LANGUAGE cpp)
    protobuf_generate(${ARGN} LANGUAGE grpc GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc PLUGIN "${GRPC_PLUGIN}")
endfunction()
