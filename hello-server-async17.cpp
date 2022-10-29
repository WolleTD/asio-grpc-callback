#include "asio-grpc.h"
#include "common.h"
#include "hello.grpc.pb.h"
#include <asio/awaitable.hpp>
#include <asio/io_context.hpp>
#include <asio/signal_set.hpp>
#include <asio/steady_timer.hpp>
#include <fmt/format.h>
#include <grpcpp/grpcpp.h>
#include <utility>

using fmt::format;
using fmt::print;
using grpc::CallbackServerContext;
using grpc::ServerUnaryReactor;
using grpc::ServerWriteReactor;
using hello::AsyncHello;
using hello::Reply;
using hello::Request;
using hello::StreamReply;
using hello::StreamRequest;

class AsyncHelloServiceImpl final : public AsyncHello::CallbackService {
public:
    using executor_type = asio::any_io_executor;

    explicit AsyncHelloServiceImpl(executor_type ex) : ex(std::move(ex)) {}

private:
    ServerUnaryReactor *greet(CallbackServerContext *ctx, const Request *request, Reply *reply) override {
        print("Server reacting async in Thread {}\n", current_thread_id());

        auto *reactor = ctx->DefaultReactor();
        auto timer_ptr = std::make_unique<asio::steady_timer>(ex);
        auto &timer = *timer_ptr;

        timer.expires_after(std::chrono::milliseconds(request->delay_ms()));
        timer.async_wait([request, reply, reactor, timer_ptr = std::move(timer_ptr)](auto ec) {
            print("asio callback in thread {}\n", current_thread_id());
            auto msg = format("Hello {}!", request->name());
            reply->set_greeting(msg);
            reactor->Finish(ec ? grpc::Status::CANCELLED : grpc::Status::OK);
        });
        return reactor;
    }

    ServerWriteReactor<StreamReply> *greet_stream(CallbackServerContext *ctx, const StreamRequest *request) override {
        print("Server reacting stream in Thread {}\n", current_thread_id());
        auto msg = format("Hello {}!", request->base().name());

        struct Greeter : ServerWriteReactor<StreamReply> {
            Greeter(const executor_type &ex, std::string message, size_t delay_ms, size_t num_replies)
                : ex(ex), timer(ex), message(std::move(message)), delay(delay_ms), num_replies(num_replies) {
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

            executor_type ex;
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
        builder.RegisterService(&async_service);

        set_grpc_server(builder.BuildAndStart());
    }

    HelloServer(HelloServer &&) noexcept = delete;
    HelloServer &operator=(HelloServer &&) noexcept = delete;

private:
    AsyncHelloServiceImpl async_service;
};

int main(int argc, const char *argv[]) {
    if (argc != 2) {
        print(stderr, "usage: {} ADDRESS[:PORT]\n", argv[0]);
        return 1;
    }

    asio::io_context ctx;

    auto server = HelloServer(ctx.get_executor(), argv[1]);
    auto sig = asio::signal_set(ctx, SIGINT, SIGTERM);
    sig.async_wait([&](auto, int) { server.shutdown(); });
    server.async_wait([](auto) { print("Server stopped\n"); });

    print("asio event loop running in {}\n", current_thread_id());
    ctx.run();
    print("asio event loop stopped\n");
}
