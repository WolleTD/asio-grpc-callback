#include "hello.grpc.pb.h"
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <fmt/format.h>
#include <grpcpp/grpcpp.h>
#include <sstream>
#include <thread>
#include <utility>

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

class AsyncHelloServiceImpl final : public AsyncHello::CallbackService {
public:
    explicit AsyncHelloServiceImpl(asio::io_context &ctx) : ctx_(ctx) {}

private:
    ServerUnaryReactor *greet(CallbackServerContext *ctx, const Request *request, Reply *reply) override {
        fmt::print("Server reacting async in Thread {}\n", current_thread_id());
        auto msg = format("Hello {}!", request->name());
        reply->set_greeting(msg);

        auto *reactor = ctx->DefaultReactor();
        auto timer_ptr = std::make_unique<asio::steady_timer>(ctx_);
        auto &timer = *timer_ptr;

        timer.expires_after(std::chrono::milliseconds(request->delay_ms()));
        timer.async_wait([reactor, timer_ptr = std::move(timer_ptr)](auto ec) {
            print("asio callback in thread {}\n", current_thread_id());
            reactor->Finish(ec ? grpc::Status::CANCELLED : grpc::Status::OK);
        });

        return reactor;
    }

    ServerWriteReactor<StreamReply> *greet_stream(CallbackServerContext *ctx, const StreamRequest *request) override {
        fmt::print("Server reacting stream in Thread {}\n", current_thread_id());
        auto msg = format("Hello {}!", request->base().name());

        struct Greeter : ServerWriteReactor<StreamReply> {
            Greeter(asio::io_context &ctx, std::string message, size_t delay_ms, size_t num_replies)
                : timer(ctx), message(std::move(message)), delay(delay_ms), num_replies(num_replies) {
                send_next();
            }

            void OnDone() override { delete this; }
            void OnWriteDone(bool) override { send_next(); }

        private:
            void send_next() {
                if (count <= num_replies) {
                    timer.expires_after(delay);
                    timer.async_wait([this](auto ec) {
                        print("asio callback in thread {}\n", current_thread_id());
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

        return new Greeter(ctx_, msg, request->base().delay_ms(), request->count());
    }

    asio::io_context &ctx_;
};

void run_server(const std::string &address, asio::io_context &ctx) {
    HelloServiceImpl service;
    auto async_service = AsyncHelloServiceImpl(ctx);

    grpc::EnableDefaultHealthCheckService(true);
    auto builder = grpc::ServerBuilder();

    builder.AddListeningPort(address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    builder.RegisterService(&async_service);

    auto server = builder.BuildAndStart();
    fmt::print("Server listening on {} (Thread {})\n", address, current_thread_id());

    server->Wait();
}

int main(int argc, const char *argv[]) {
    if (argc != 2) {
        print(stderr, "usage: {} ADDRESS[:PORT]\n", argv[0]);
        return 1;
    }

    asio::io_context ctx;

    std::thread([&]() {
        [[maybe_unused]] auto work = asio::require(ctx.get_executor(), asio::execution::outstanding_work_t::tracked);
        print("asio event loop running in {}\n", current_thread_id());
        ctx.run();
    }).detach();

    run_server(argv[1], ctx);
}
