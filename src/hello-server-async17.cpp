#include "common.h"
#include "hello-server.h"
#include "hello.grpc.pb.h"
#include <asio/awaitable.hpp>
#include <asio/steady_timer.hpp>
#include <fmt/format.h>
#include <grpcpp/grpcpp.h>
#include <utility>

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

class HelloServiceImpl final : public Hello::CallbackService {
public:
    using executor_type = asio::any_io_executor;

    explicit HelloServiceImpl(executor_type ex) : ex(std::move(ex)) {}

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

    ServerWriteReactor<StreamReply> *greet_stream(CallbackServerContext *, const StreamRequest *request) override {
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

HelloServer::HelloServer(const asio::any_io_executor &ex, const std::string &address)
    : asio_grpc::Server(ex), service(new HelloServiceImpl(ex)) {
    grpc::EnableDefaultHealthCheckService(true);
    auto builder = grpc::ServerBuilder();

    builder.AddListeningPort(address, grpc::InsecureServerCredentials());
    builder.RegisterService(service.get());

    set_grpc_server(builder.BuildAndStart());
}

HelloServer::~HelloServer() = default;
