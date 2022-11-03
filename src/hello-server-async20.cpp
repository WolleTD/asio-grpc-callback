#include "common.h"
#include "hello-server.h"
#include "hello.grpc.pb.h"
#include <asio/awaitable.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/steady_timer.hpp>
#include <fmt/format.h>
#include <grpcpp/grpcpp.h>
#include <utility>

#ifdef USE_ASIO_CORO

#include <asio/experimental/coro.hpp>
#include <asio/experimental/use_coro.hpp>

#endif

using asio::awaitable;
using asio::use_awaitable;
#ifdef USE_ASIO_CORO
using asio::experimental::coro;
using asio::experimental::use_coro;
#endif
using asio_grpc::asio_server_unary_reactor;
using asio_grpc::asio_server_write_reactor;
using asio_grpc::default_reactor_handler;
using fmt::format;
using fmt::print;
using grpc::CallbackServerContext;
using grpc::ServerUnaryReactor;
using grpc::ServerWriteReactor;
using hello::Hello;
using hello::Reply;
using hello::Request;
using hello::StreamReply;
using hello::StreamRequest;
using std::nullopt;
using std::optional;

auto greet_coro(const Request *request, Reply *reply) -> awaitable<grpc::Status> {
    auto msg = format("Hello {}!", request->name());
    reply->set_greeting(msg);

    auto timer = asio::steady_timer(co_await asio::this_coro::executor);

    timer.expires_after(std::chrono::milliseconds(request->delay_ms()));
    co_await timer.async_wait(asio::use_awaitable);
    print("asio callback for {} in a coro thread {}\n", request->name(), current_thread_id());
    co_return grpc::Status::OK;
}

class HelloServiceImpl final : public Hello::CallbackService {
public:
    using executor_type = asio::any_io_executor;

    explicit HelloServiceImpl(executor_type ex) : ex(std::move(ex)) {}

private:
    ServerUnaryReactor *greet(CallbackServerContext *, const Request *request, Reply *reply) override {
        print("Server reacting async in Thread {}\n", current_thread_id());

        auto *reactor = new asio_server_unary_reactor(ex);
        co_spawn(ex, greet_coro(request, reply), default_reactor_handler(reactor));
        return reactor;
    }

    ServerWriteReactor<StreamReply> *greet_stream(CallbackServerContext *, const StreamRequest *request) override {
        print("Server reacting stream in Thread {}\n", current_thread_id());
        auto msg = format("Hello {}!", request->base().name());

        struct Greeter : asio_server_write_reactor<StreamReply> {
            using base = asio_server_write_reactor<StreamReply>;

            Greeter(const executor_type &ex, std::string message, size_t delay_ms, size_t num_replies)
#if defined USE_ASIO_CORO
                : base(ex), generator(hello_generator(std::move(message), delay_ms, num_replies)) {
#else
                : base(ex), timer(ex), message(std::move(message)), delay(delay_ms), num_replies(num_replies) {
#endif
                send_next();
            }

        private:
            void send_next() final { co_spawn(get_executor(), send_next_impl(), default_reactor_handler(this)); }

#ifdef USE_ASIO_CORO

            awaitable<optional<grpc::Status>> send_next_impl() {
                if (auto reply = co_await generator.async_resume(use_awaitable)) {
                    StartWrite(*reply);
                    co_return nullopt;
                } else {
                    co_return grpc::Status::OK;
                }
            }

            coro<const StreamReply *> hello_generator(std::string message, size_t delay_ms, size_t num_replies) {
                asio::steady_timer timer(co_await asio::this_coro::executor);
                StreamReply reply;
                reply.set_greeting(message);

                for (size_t i = 1; i <= num_replies; i++) {
                    timer.expires_after(std::chrono::milliseconds(delay_ms));
                    co_await timer.async_wait(use_coro);
                    print("asio stream dank coro callback in thread {}\n", current_thread_id());
                    reply.set_count(i);
                    co_yield &reply;
                }
            }

#else
            awaitable<optional<grpc::Status>> send_next_impl() {
                if (count <= num_replies) {
                    timer.expires_after(delay);
                    co_await timer.async_wait(use_awaitable);
                    print("asio stream coro callback in thread {}\n", current_thread_id());
                    StreamReply reply;
                    reply.set_greeting(message);
                    reply.set_count(static_cast<int32_t>(count));
                    count += 1;
                    StartWrite(&reply);
                    co_return nullopt;
                } else {
                    co_return grpc::Status::OK;
                }
            }
#endif

#if defined USE_ASIO_CORO
            coro<const StreamReply *> generator;
#else
            asio::steady_timer timer;
            std::string message;
            std::chrono::milliseconds delay;
            size_t count = 1;
            size_t num_replies;
#endif
        };

        return new Greeter(ex, msg, request->base().delay_ms(), request->count());
    }

    executor_type ex;
};

HelloServer::HelloServer(const asio::any_io_executor &ex, const std::string &address)
    : asio_grpc::Server(ex), service(new HelloServiceImpl(ex)) {
    grpc::EnableDefaultHealthCheckService(true);
    auto builder = grpc::ServerBuilder();

    builder.AddListeningPort(address, grpc::InsecureServerCredentials());
    builder.RegisterService(service.get());

    set_grpc_server(builder.BuildAndStart());
}

HelloServer::~HelloServer() = default;
