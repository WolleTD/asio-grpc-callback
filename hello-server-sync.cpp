#include "common.h"
#include "hello-server.h"
#include "hello.grpc.pb.h"
#include <fmt/format.h>
#include <grpcpp/grpcpp.h>

using fmt::format;
using fmt::print;
using grpc::ServerContext;
using hello::Hello;
using hello::Reply;
using hello::Request;
using hello::StreamReply;
using hello::StreamRequest;

class HelloServiceImpl final : public Hello::Service {
    grpc::Status greet(ServerContext *ctx, const Request *request, Reply *reply) override {
        print("Server reacting in Thread {}\n", current_thread_id());
        auto msg = format("Hello {}!", request->name());
        reply->set_greeting(msg);

        return grpc::Status::OK;
    }
};

HelloServer::HelloServer(const asio::any_io_executor &ex, const std::string &address)
    : asio_grpc::Server(ex), service(new HelloServiceImpl()) {
    grpc::EnableDefaultHealthCheckService(true);
    auto builder = grpc::ServerBuilder();

    builder.AddListeningPort(address, grpc::InsecureServerCredentials());
    builder.RegisterService(service.get());

    set_grpc_server(builder.BuildAndStart());
}

HelloServer::~HelloServer() = default;
