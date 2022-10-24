#include "asio-grpc.h"

#include "hello.grpc.pb.h"
#include <asio/bind_executor.hpp>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/thread_pool.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>
#include <fmt/format.h>
#include <grpcpp/grpcpp.h>
#include <sstream>
#include <thread>
#include <utility>

#ifdef __cpp_impl_coroutine
using asio::awaitable;
using asio::use_awaitable;
#endif
using asio_grpc::async_initiate_grpc;
using asio_grpc::StreamChannel;
using fmt::format;
using fmt::print;
using grpc::Channel;
using hello::AsyncHello;
using hello::Hello;
using hello::Reply;
using hello::Request;
using hello::StreamReply;
using hello::StreamRequest;

std::string current_thread_id() {
    std::stringstream ss;
    ss << std::this_thread::get_id();
    return ss.str();
}

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

// While we are symmetrically using the sync/async APIs on client and server in this example, this
// isn't required and each side can use whatever API they like to.
// We connect the methods of this client to asio by using the CompletionToken boilerplate.
class AsyncHelloClient {
public:
    explicit AsyncHelloClient(asio::io_context &ctx, const std::shared_ptr<Channel> &channel)
        : ctx_(ctx), stub_(AsyncHello::NewStub(channel)) {}

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
    std::unique_ptr<AsyncHello::Stub> stub_;
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

#ifdef __cpp_impl_coroutine
void run_cpp20(const std::string &addr, const std::string &name) {
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
    print("Starting off with a synchronous request...\n");
    auto client = HelloClient(grpc::CreateChannel(addr, grpc::InsecureChannelCredentials()));
    auto reply = client.greet(makeRequest(name, 0));
    print("Received sync response: {}\n", reply.greeting());

    print("That worked, now let's throw some async in there!\n");
    auto a_client = AsyncHelloClient(ctx, grpc::CreateChannel(addr, grpc::InsecureChannelCredentials()));

    auto error_handler = [](const std::exception_ptr &eptr) {
        try {
            if (eptr) { std::rethrow_exception(eptr); }
        } catch (std::exception &e) { print(stderr, "Coroutine ended with exception: {}\n", e.what()); }
    };

    auto coro1 = [&]() -> awaitable<void> {
        auto request = makeRequest(name, 1000);
        auto reply = co_await a_client.greet(request, use_awaitable);
        auto tid = current_thread_id();
        print("Received async response 1 in thread {} ({}): {}\n", tid, tid == tp_tid, reply.greeting());
    };

    auto coro2 = [&]() -> awaitable<void> {
        auto request = makeRequest(name, 2000);
        auto reply = co_await a_client.greet(request, use_awaitable);
        auto tid = current_thread_id();
        print("Received async response 2 in thread {} ({}): {}\n", tid, tid == ctx_tid, reply.greeting());
        print("Restarting timer!\n");

        // This will run last!
        timer.expires_after(std::chrono::milliseconds(1000));
        co_await timer.async_wait(use_awaitable);
        print("Timer started in response 2 expired. Reply was: {}\n", reply.greeting());
    };

    auto coro3 = [&]() -> awaitable<void> {
        timer.expires_after(std::chrono::milliseconds(500));
        co_await timer.async_wait(use_awaitable);
        auto coro3_1 = [&]() -> awaitable<void> {
            auto request = makeRequest(name, 1000);
            auto reply = co_await a_client.greet(request, use_awaitable);
            auto tid = current_thread_id();
            print("Received async response 3 in thread {} ({}): {}\n", tid, tid == tp_tid, reply.greeting());
        };
        co_spawn(tp, coro3_1, error_handler);
        auto stream = a_client.greet_stream(makeStreamRequest(name, 100, 10));
        int i = 1;
        for (;;) {
            auto sreply = co_await stream->read_next(use_awaitable);
            auto tid = current_thread_id();
            print("Received stream response {} ({}) in thread {} ({}): {}\n", sreply.count(), i++, tid, tid == ctx_tid,
                  sreply.greeting());
        }
    };

    co_spawn(tp, coro1, error_handler);
    co_spawn(ctx, coro2, error_handler);
    co_spawn(ctx, coro3, error_handler);

    ctx.run();
}
#endif

void run_cpp17(const std::string &addr, const std::string &name) {
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
    print("Starting off with a synchronous request...\n");
    auto client = HelloClient(grpc::CreateChannel(addr, grpc::InsecureChannelCredentials()));
    auto reply = client.greet(makeRequest(name, 0));
    print("Received sync response: {}\n", reply.greeting());

    print("That worked, now let's throw some async in there!\n");
    auto a_client = AsyncHelloClient(ctx, grpc::CreateChannel(addr, grpc::InsecureChannelCredentials()));

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
        void operator()(const std::exception_ptr &eptr, const StreamReply &reply) {
            if (!eptr) {
                auto tid = current_thread_id();
                print("Received stream response {} ({}) in thread {} ({}): {}\n", reply.count(), i++, tid,
                      tid == ctx_tid, reply.greeting());
                stream->read_next(std::move(*this));
            } else {
                try {
                    std::rethrow_exception(eptr);
                } catch (std::runtime_error &e) { print("Stream error: {} i = {}\n", e.what(), i); }
            }
        }

        std::unique_ptr<AsyncHelloClient::GreetStream> stream;
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

int main(int argc, const char *argv[]) {
    if (argc != 3) {
        print(stderr, "usage: {} ADDRESS[:PORT] NAME\n", argv[0]);
        return 1;
    }

    auto addr = std::string(argv[1]);
    auto name = std::string(argv[2]);

    run_cpp17(addr, name);
#ifdef __cpp_impl_coroutine
    run_cpp20(addr, name);
#endif
}
