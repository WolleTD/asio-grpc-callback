#include "asio-grpc.h"
#include "common.h"
#include "hello-client.h"
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

void run(const std::string &addr, const std::string &name) {

    print("Starting a synchronous request...\n");
    auto client = HelloClient(grpc::CreateChannel(addr, grpc::InsecureChannelCredentials()));
    auto reply = client.greet(makeRequest(name, 0));
    print("Received sync response: {}\n", reply.greeting());
}
