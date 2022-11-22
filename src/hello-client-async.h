#include "asio-grpc-client.h"
#include "hello.grpc.pb.h"
#include <asio/io_context.hpp>
#include <grpcpp/channel.h>
#include <utility>

using asio_grpc::async_initiate_grpc;
using asio_grpc::StreamChannel;
using grpc::Channel;
using hello::Hello;
using hello::Reply;
using hello::Request;
using hello::StreamReply;
using hello::StreamRequest;

class HelloClient {
public:
    explicit HelloClient(asio::io_context &ctx, const std::shared_ptr<Channel> &channel)
        : ctx_(ctx), stub_(Hello::NewStub(channel)) {}

    template<typename CompletionToken>
    auto greet(const Request &request, CompletionToken &&token) {
        return async_initiate_grpc<Reply>(
                ctx_, token, [this, &request](grpc::ClientContext *ctx, Reply *reply, auto &&handler) {
                    stub_->async()->greet(ctx, &request, reply, std::forward<decltype(handler)>(handler));
                });
    }

    using GreetStream = StreamChannel<StreamReply>;

    GreetStream greet_stream(const StreamRequest &request) {
        return {ctx_,
                [this, &request](auto *ctx, auto *reactor) { stub_->async()->greet_stream(ctx, &request, reactor); }};
    }

private:
    asio::io_context &ctx_;
    std::unique_ptr<Hello::Stub> stub_;
};
