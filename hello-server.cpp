#include "asio-grpc.h"
#include "hello.grpc.pb.h"
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/signal_set.hpp>
#include <asio/steady_timer.hpp>
#include <fmt/format.h>
#include <grpcpp/grpcpp.h>
#include <sstream>
#include <thread>
#include <utility>

#ifdef __cpp_impl_coroutine
using asio::awaitable;
using asio::use_awaitable;
#endif
using fmt::format;
using fmt::print;
using grpc::CallbackServerContext;
using grpc::ServerContext;
using grpc::ServerUnaryReactor;
using grpc::ServerWriter;
using grpc::ServerWriteReactor;
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

class HelloServiceImpl final : public Hello::Service {
    grpc::Status greet(ServerContext *ctx, const Request *request, Reply *reply) override {
        fmt::print("Server reacting in Thread {}\n", current_thread_id());
        auto msg = format("Hello {}!", request->name());
        reply->set_greeting(msg);

        return grpc::Status::OK;
    }
};

#ifdef __cpp_impl_coroutine
struct unary_coro_handler {
    explicit unary_coro_handler(ServerUnaryReactor *reactor) : reactor_(reactor) {}

    void operator()(const std::exception_ptr &eptr, const grpc::Status &status) {
        try {
            if (eptr) { std::rethrow_exception(eptr); }
            reactor_->Finish(status);
        } catch (std::exception &e) {
            print("Coroutine exception: {}\n", e.what());
            reactor_->Finish(grpc::Status::CANCELLED);
        } catch (...) {
            print("Catched something not an excpetion!\n");
            reactor_->Finish(grpc::Status::CANCELLED);
        }
    }

private:
    ServerUnaryReactor *reactor_;
};

auto greet_coro(const Request *request, Reply *reply) -> awaitable<grpc::Status> {
    auto msg = format("Hello {}!", request->name());
    reply->set_greeting(msg);

    auto timer = asio::steady_timer(co_await asio::this_coro::executor);

    timer.expires_after(std::chrono::milliseconds(request->delay_ms()));
    co_await timer.async_wait(asio::use_awaitable);
    print("asio callback in a coro thread {}\n", current_thread_id());
    co_return grpc::Status::OK;
}
#endif

class AsyncHelloServiceImpl final : public AsyncHello::CallbackService {
public:
    using executor_type = asio::any_io_executor;

    explicit AsyncHelloServiceImpl(executor_type ex) : ex(std::move(ex)) {}

private:
    ServerUnaryReactor *greet(CallbackServerContext *ctx, const Request *request, Reply *reply) override {
        fmt::print("Server reacting async in Thread {}\n", current_thread_id());

        auto *reactor = ctx->DefaultReactor();
#ifdef __cpp_impl_coroutine
        co_spawn(ex, greet_coro(request, reply), unary_coro_handler(reactor));
#else
        auto timer_ptr = std::make_unique<asio::steady_timer>(ex);
        auto &timer = *timer_ptr;

        timer.expires_after(std::chrono::milliseconds(request->delay_ms()));
        timer.async_wait([request, reply, reactor, timer_ptr = std::move(timer_ptr)](auto ec) {
            print("asio callback in thread {}\n", current_thread_id());
            auto msg = format("Hello {}!", request->name());
            reply->set_greeting(msg);
            reactor->Finish(ec ? grpc::Status::CANCELLED : grpc::Status::OK);
        });
#endif
        return reactor;
    }

    ServerWriteReactor<StreamReply> *greet_stream(CallbackServerContext *ctx, const StreamRequest *request) override {
        fmt::print("Server reacting stream in Thread {}\n", current_thread_id());
        auto msg = format("Hello {}!", request->base().name());

        struct Greeter : ServerWriteReactor<StreamReply> {
            Greeter(const executor_type &ex, std::string message, size_t delay_ms, size_t num_replies)
                : timer(ex), message(std::move(message)), delay(delay_ms), num_replies(num_replies) {
                send_next();
            }

            void OnDone() override { delete this; }

            void OnWriteDone(bool ok) override {
                if (ok) {
                    send_next();
                } else {
                    Finish(grpc::Status::CANCELLED);
                }
            }

        private:
            void send_next() {
                if (count <= num_replies) {
                    timer.expires_after(delay);
                    timer.async_wait([this](auto ec) {
                        print("asio stream callback in thread {}\n", current_thread_id());
                        if (ec) {
                            Finish(grpc::Status::CANCELLED);
                        } else {
                            StreamReply reply;
                            reply.set_greeting(message);
                            reply.set_count(static_cast<int32_t>(count));
                            count += 1;
                            StartWrite(&reply);
                        }
                    });
                } else {
                    Finish(grpc::Status::OK);
                }
            }

            asio::steady_timer timer;
            std::string message;
            std::chrono::milliseconds delay;
            size_t count = 1;
            size_t num_replies;
        };

        return new Greeter(ex, msg, request->base().delay_ms(), request->count());
    }

    executor_type ex;
};

struct HelloServer : asio_grpc::Server {
    template<typename Executor, typename = asio_grpc::enable_for_executor_t<Executor>>
    HelloServer(Executor ex, const std::string &address) : asio_grpc::Server(ex), async_service(ex) {
        grpc::EnableDefaultHealthCheckService(true);
        auto builder = grpc::ServerBuilder();

        builder.AddListeningPort(address, grpc::InsecureServerCredentials());
        builder.RegisterService(&sync_service);
        builder.RegisterService(&async_service);

        set_grpc_server(builder.BuildAndStart());
    }

    HelloServer(HelloServer &&) noexcept = delete;
    HelloServer &operator=(HelloServer &&) noexcept = delete;

private:
    HelloServiceImpl sync_service;
    AsyncHelloServiceImpl async_service;
};

#ifdef __cpp_impl_coroutine
awaitable<void> run_server(std::string addr) {
    auto ex = co_await asio::this_coro::executor;
    auto server = HelloServer(ex, addr);
    print("Waiting for clients...\n");

    auto sig = asio::signal_set(ex, SIGINT, SIGTERM);
    sig.async_wait([&server](auto, int) {
        print("Shutting down server\n");
        server.shutdown();
    });

    co_await server.async_wait(use_awaitable);
    print("Server stopped\n");
}
#endif

int main(int argc, const char *argv[]) {
    if (argc != 2) {
        print(stderr, "usage: {} ADDRESS[:PORT]\n", argv[0]);
        return 1;
    }

    asio::io_context ctx;

#ifdef __cpp_impl_coroutine
    co_spawn(ctx, run_server(argv[1]), asio::detached);
#else
    auto server = HelloServer(ctx.get_executor(), argv[1]);
    auto sig = asio::signal_set(ctx, SIGINT, SIGTERM);
    sig.async_wait([&](auto, int) { server.shutdown(); });
    server.async_wait([](auto) { print("Server stopped\n"); });
#endif

    print("asio event loop running in {}\n", current_thread_id());
    ctx.run();
    print("asio event loop stopped\n");
}
