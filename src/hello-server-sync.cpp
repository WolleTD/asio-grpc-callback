// hello-server-sync.cpp
//
// SPDX-FileCopyrightText: 2022 Eicke Herbertz
// SPDX-License-Identifier: MIT

#include "common.h"
#include "hello-server.h"
#include "hello.grpc.pb.h"
#include <fmt/format.h>
#include <grpcpp/grpcpp.h>

using fmt::format;
using fmt::print;
using grpc::ServerContext;
using grpc::ServerWriter;
using hello::Hello;
using hello::Reply;
using hello::Request;
using hello::StreamReply;
using hello::StreamRequest;

class HelloServiceImpl final : public Hello::Service {
    grpc::Status greet(ServerContext *, const Request *request, Reply *reply) override {
        print("Server reacting in Thread {}\n", current_thread_id());
        auto msg = format("Hello {}!", request->name());
        reply->set_greeting(msg);
        std::this_thread::sleep_for(std::chrono::milliseconds(request->delay_ms()));

        return grpc::Status::OK;
    }

    grpc::Status greet_stream(ServerContext *, const StreamRequest *request,
                              ServerWriter<StreamReply> *writer) override {
        print("Server reacting stream in Thread {}\n", current_thread_id());
        auto msg = format("Hello {}!", request->base().name());
        auto delay = std::chrono::milliseconds(request->base().delay_ms());
        auto count = request->count();

        StreamReply reply;
        reply.set_greeting(msg);

        for (int32_t i = 1; i <= count; i++) {
            std::this_thread::sleep_for(delay);
            print("asio stream send in thread {}\n", current_thread_id());
            reply.set_count(i);
            if (not writer->Write(reply)) { return grpc::Status::CANCELLED; }
        }

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
