#include "common.h"
#include "hello-server.h"
#include "hello.grpc.pb.h"
#include <asio/awaitable.hpp>
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

template<typename T>
concept GrpcReactor = requires(T *t) { t->Finish(grpc::Status{}); };

template<GrpcReactor Reactor>
struct reactor_coro_handler {
    explicit reactor_coro_handler(Reactor *reactor) : reactor_(reactor) {}

    void operator()(const std::exception_ptr &eptr, const std::optional<grpc::Status> &status) {
        try {
            if (eptr) { std::rethrow_exception(eptr); }
            if (status) { reactor_->Finish(*status); }
        } catch (std::exception &e) {
            print("Coroutine exception: {}\n", e.what());
            reactor_->Finish(grpc::Status::CANCELLED);
        } catch (...) {
            print("Catched something not an excpetion!\n");
            reactor_->Finish(grpc::Status::CANCELLED);
        }
    }

private:
    Reactor *reactor_;
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

class HelloServiceImpl final : public Hello::CallbackService {
public:
    using executor_type = asio::any_io_executor;

    explicit HelloServiceImpl(executor_type ex) : ex(std::move(ex)) {}

private:
    ServerUnaryReactor *greet(CallbackServerContext *ctx, const Request *request, Reply *reply) override {
        print("Server reacting async in Thread {}\n", current_thread_id());

        auto *reactor = ctx->DefaultReactor();
        co_spawn(ex, greet_coro(request, reply), reactor_coro_handler(reactor));
        return reactor;
    }

    ServerWriteReactor<StreamReply> *greet_stream(CallbackServerContext *, const StreamRequest *request) override {
        print("Server reacting stream in Thread {}\n", current_thread_id());
        auto msg = format("Hello {}!", request->base().name());

        struct Greeter : ServerWriteReactor<StreamReply> {
            Greeter(const executor_type &ex, std::string message, size_t delay_ms, size_t num_replies)
#if defined USE_ASIO_CORO
                : ex(ex), generator(hello_generator(std::move(message), delay_ms, num_replies)) {
#else
                : ex(ex), timer(ex), message(std::move(message)), delay(delay_ms), num_replies(num_replies) {
#endif
                send_next();
            }

            [[nodiscard]] executor_type get_executor() const { return ex; }

            void OnDone() override { delete this; }

            void OnWriteDone(bool ok) override {
                if (ok) {
                    send_next();
                } else {
                    Finish(grpc::Status::CANCELLED);
                }
            }

        private:
            void send_next() { co_spawn(ex, send_next_impl(), reactor_coro_handler(this)); }

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

            executor_type ex;
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
