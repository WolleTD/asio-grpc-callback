#include "asio-grpc-client.h"
#include "common.h"
#include "hello-client.h"
#include <asio/bind_executor.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/thread_pool.hpp>
#include <asio/use_future.hpp>
#include <fmt/format.h>
#include <grpcpp/grpcpp.h>
#include <utility>

using asio_grpc::async_initiate_grpc;
using asio_grpc::StreamChannel;
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

    std::unique_ptr<GreetStream> greet_stream(const StreamRequest &request) {
        return std::make_unique<GreetStream>(ctx_, [this, &request](auto *ctx, auto *reactor) {
            stub_->async()->greet_stream(ctx, &request, reactor);
        });
    }

private:
    asio::io_context &ctx_;
    std::unique_ptr<Hello::Stub> stub_;
};

void run(const std::string &addr, const std::string &name) {
    asio::io_context ctx;
    asio::thread_pool tp{1};
    asio::steady_timer timer(ctx);

    auto ctx_tid = current_thread_id();
    print("Main thread id is: {}\n", current_thread_id());
    auto p = std::promise<std::string>();
    auto f = p.get_future();
    asio::post(tp, [&p]() {
        auto tid = current_thread_id();
        p.set_value(tid);
        print("Thread pool thread is: {}\n", tid);
    });
    auto tp_tid = f.get();
    print("Starting some async requests!\n");

    auto a_client = HelloClient(ctx, grpc::CreateChannel(addr, grpc::InsecureChannelCredentials()));

    a_client.greet(makeRequest(name, 1000),
                   asio::bind_executor(tp, [&](const std::exception_ptr &, const Reply &reply) {
                       auto tid = current_thread_id();
                       print("Received async response 1 in thread {} ({}): {}\n", tid, tid == tp_tid, reply.greeting());
                   }));

    a_client.greet(makeRequest(name, 2000), [&](const std::exception_ptr &, const Reply &reply) {
        auto tid = current_thread_id();
        print("Received async response 2 in thread {} ({}): {}\n", tid, tid == ctx_tid, reply.greeting());
        print("Restarting timer!\n");

        // This will run last!
        timer.expires_after(std::chrono::milliseconds(1000));
        timer.async_wait(
                [=](auto) { print("Timer started in response 2 expired. Reply was: {}\n", reply.greeting()); });
    });

    struct Reader {
        void operator()(const std::exception_ptr &eptr, const std::optional<StreamReply> &reply) {
            if (!eptr and reply) {
                auto tid = current_thread_id();
                print("Received stream response {} ({}) in thread {} ({}): {}\n", reply->count(), i++, tid,
                      tid == ctx_tid, reply->greeting());
                stream->read_next(std::move(*this));
            } else if (not reply) {
                print("Stream terminated gracefully\n");
            } else {
                try {
                    std::rethrow_exception(eptr);
                } catch (std::runtime_error &e) { print("Stream error: {} i = {}\n", e.what(), i); }
            }
        }

        std::unique_ptr<HelloClient::GreetStream> stream;
        const std::string &ctx_tid;
        int i = 1;
    };

    timer.expires_after(std::chrono::milliseconds(500));
    timer.async_wait([&](auto) {
        a_client.greet(
                makeRequest(name, 1000), asio::bind_executor(tp, [&](const std::exception_ptr &, const Reply &reply) {
                    auto tid = current_thread_id();
                    print("Received async response 3 in thread {} ({}): {}\n", tid, tid == tp_tid, reply.greeting());
                }));
        auto stream = a_client.greet_stream(makeStreamRequest(name, 100, 10));
        auto &stream_ref = *stream;
        stream_ref.read_next(Reader{std::move(stream), ctx_tid});
    });
    ctx.run();
}
