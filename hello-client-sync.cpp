#include "asio-grpc.h"
#include "common.h"
#include "hello.grpc.pb.h"
#include <fmt/format.h>
#include <grpcpp/grpcpp.h>

using fmt::format;
using fmt::print;
using grpc::Channel;
using hello::Hello;
using hello::Reply;
using hello::Request;
using hello::StreamReply;
using hello::StreamRequest;

class HelloClient {
public:
    explicit HelloClient(const std::shared_ptr<Channel> &channel) : stub_(Hello::NewStub(channel)) {}

    Reply greet(const Request &request) {
        auto reply = Reply();
        auto ctx = grpc::ClientContext();

        auto status = stub_->greet(&ctx, request, &reply);

        if (status.ok()) {
            return reply;
        } else {
            throw asio_grpc::grpc_error(status);
        }
    }

private:
    std::unique_ptr<Hello::Stub> stub_;
};

Request makeRequest(const std::string &name, int32_t delay_ms) {
    Request request;
    request.set_name(name);
    request.set_delay_ms(delay_ms);
    return request;
}

StreamRequest makeStreamRequest(const std::string &name, int32_t delay_ms, int32_t num_replies) {
    StreamRequest request;
    request.mutable_base()->set_name(name);
    request.mutable_base()->set_delay_ms(delay_ms);
    request.set_count(num_replies);
    return request;
}

void run(const std::string &addr, const std::string &name) {

    print("Starting a synchronous request...\n");
    auto client = HelloClient(grpc::CreateChannel(addr, grpc::InsecureChannelCredentials()));
    auto reply = client.greet(makeRequest(name, 0));
    print("Received sync response: {}\n", reply.greeting());
}

int main(int argc, const char *argv[]) {
    if (argc != 3) {
        print(stderr, "usage: {} ADDRESS[:PORT] NAME\n", argv[0]);
        return 1;
    }

    auto addr = std::string(argv[1]);
    auto name = std::string(argv[2]);

    run(addr, name);
}
