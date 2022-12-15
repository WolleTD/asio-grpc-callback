// hello-client-sync.cpp
//
// SPDX-FileCopyrightText: 2022 Eicke Herbertz
// SPDX-License-Identifier: MIT

#include "asio-grpc-client.h"
#include "common.h"
#include "hello-client.h"
#include <fmt/format.h>
#include <grpcpp/grpcpp.h>

using fmt::format;
using fmt::print;
using grpc::Channel;
using grpc::ClientContext;
using grpc::ClientReader;
using hello::Hello;
using hello::Reply;
using hello::Request;
using hello::StreamReply;
using hello::StreamRequest;

struct GreetStream {
    std::unique_ptr<ClientContext> ctx;
    std::unique_ptr<ClientReader<StreamReply>> reader;
};

class HelloClient {
public:
    explicit HelloClient(const std::shared_ptr<Channel> &channel) : stub_(Hello::NewStub(channel)) {}

    Reply greet(const Request &request) {
        auto reply = Reply();
        auto ctx = ClientContext();

        auto status = stub_->greet(&ctx, request, &reply);

        if (status.ok()) {
            return reply;
        } else {
            throw asio_grpc::grpc_error(status);
        }
    }

    GreetStream greet_stream(const StreamRequest &request) {
        auto r = GreetStream{std::make_unique<ClientContext>(), nullptr};
        r.reader = stub_->greet_stream(r.ctx.get(), request);
        return r;
    }

private:
    std::unique_ptr<Hello::Stub> stub_;
};

void run(const std::string &addr, const std::string &name) {

    print("Starting a synchronous request...\n");
    auto client = HelloClient(grpc::CreateChannel(addr, grpc::InsecureChannelCredentials()));
    auto reply = client.greet(makeRequest(name, 0));
    print("Received sync response: {}\n", reply.greeting());
    auto stream = client.greet_stream(makeStreamRequest(name, 100, 10));
    StreamReply stream_reply{};
    while (stream.reader->Read(&stream_reply)) {
        print("Received stream response {}: {}\n", stream_reply.count(), stream_reply.greeting());
    }
    auto status = stream.reader->Finish();
    if (not status.ok()) { print("Stream ended with status {}\n", status.error_message()); }
}
