#include "common.h"
#include "hello-server.h"
#include "hello.grpc.pb.h"
#include <asio/awaitable.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/steady_timer.hpp>
#include <fmt/format.h>
#include <grpcpp/grpcpp.h>
#include <utility>

using asio::bind_cancellation_slot;
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

class HelloServiceImpl final : public Hello::CallbackService {
public:
    using executor_type = asio::any_io_executor;

    explicit HelloServiceImpl(executor_type ex) : ex(std::move(ex)) {}

private:
    ServerUnaryReactor *greet(CallbackServerContext *, const Request *request, Reply *reply) override {
        print("Server reacting async in Thread {}\n", current_thread_id());

        auto reactor = new asio_server_unary_reactor(ex);
        auto timer_ptr = std::make_unique<asio::steady_timer>(ex);
        auto &timer = *timer_ptr;

        auto handler = default_reactor_handler(reactor);
        auto cs = asio::get_associated_cancellation_slot(handler);

        timer.expires_after(std::chrono::milliseconds(request->delay_ms()));
        timer.async_wait(
                bind_cancellation_slot(cs, [request, reply, handler, timer_ptr = std::move(timer_ptr)](auto ec) {
                    if (not ec) {
                        print("asio callback in thread {}\n", current_thread_id());
                        auto msg = format("Hello {}!", request->name());
                        reply->set_greeting(msg);
                    }
                    handler(ec, grpc::Status::OK);
                }));
        return reactor;
    }

    struct Greeter : asio_server_write_reactor<StreamReply> {
        using base = asio_server_write_reactor<StreamReply>;

        Greeter(const executor_type &ex, std::string message, size_t delay_ms, size_t num_replies)
            : base(ex), timer(ex), message(std::move(message)), delay(delay_ms), num_replies(num_replies) {
            send_next();
        }

    private:
        void send_next() final { send_next_impl(default_reactor_handler(this)); }

        template<typename Handler>
        void send_next_impl(Handler handler) {
            if (count <= num_replies) {
                auto cs = asio::get_associated_cancellation_slot(handler);
                timer.expires_after(delay);
                timer.async_wait(bind_cancellation_slot(cs, [this, handler](auto ec) {
                    if (not ec) {
                        print("asio stream callback in thread {}\n", current_thread_id());
                        StreamReply reply;
                        reply.set_greeting(message);
                        reply.set_count(static_cast<int32_t>(count));
                        count += 1;
                        StartWrite(&reply);
                    }
                    // if ec contains an error, the handler shall cancel the operation
                    handler(ec, std::nullopt);
                }));
            } else {
                handler(nullptr, grpc::Status::OK);
            }
        }

        asio::steady_timer timer;
        std::string message;
        std::chrono::milliseconds delay;
        size_t count = 1;
        size_t num_replies;
    };

    ServerWriteReactor<StreamReply> *greet_stream(CallbackServerContext *, const StreamRequest *request) override {
        print("Server reacting stream in Thread {}\n", current_thread_id());
        auto msg = format("Hello {}!", request->base().name());

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
