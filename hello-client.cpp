#include "hello.grpc.pb.h"
#include <asio/bind_executor.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/thread_pool.hpp>
#include <fmt/format.h>
#include <future>
#include <grpcpp/grpcpp.h>
#include <sstream>
#include <thread>

using fmt::format;
using fmt::print;
using grpc::Channel;
using grpc::ClientContext;
using hello::AsyncHello;
using hello::Hello;
using hello::Reply;
using hello::Request;
using hello::StreamRequest;

std::string current_thread_id() {
    std::stringstream ss;
    ss << std::this_thread::get_id();
    return ss.str();
}

class grpc_error : public std::runtime_error {
public:
    explicit grpc_error(const grpc::Status &status)
        : std::runtime_error(status.error_message()), code_(status.error_code()) {}

    [[nodiscard]] grpc::StatusCode error_code() const { return code_; }

private:
    grpc::StatusCode code_;
};

std::exception_ptr to_exception_ptr(const grpc::Status &status) {
    return status.ok() ? std::exception_ptr{} : std::make_exception_ptr(grpc_error(status));
}

class HelloClient {
public:
    explicit HelloClient(const std::shared_ptr<Channel> &channel) : stub_(Hello::NewStub(channel)) {}

    Reply greet(const Request &request) {
        auto reply = Reply();
        auto ctx = ClientContext();

        auto status = stub_->greet(&ctx, request, &reply);

        if (status.ok()) {
            return reply;
        } else {
            throw grpc_error(status);
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
        // Keeps the io_context active, we are basically an io-object for asio
        auto work = asio::make_work_guard(ctx_);

        auto initiation = [this, work](auto &&handler, const Request &request) {
            print("asio initiation is running in thread {}\n", current_thread_id());

            auto reply = std::make_shared<Reply>();
            auto ctx = std::make_shared<ClientContext>();

            stub_->async()->greet(
                    ctx.get(), &request, reply.get(),
                    [reply, ctx, work,
                     handler = std::forward<decltype(handler)>(handler)](const grpc::Status &status) mutable {
                        print("gRPC handler is running in thread {}\n", current_thread_id());
                        auto ex = asio::get_associated_executor(handler, work.get_executor());
                        asio::post(ex, [status, reply, handler = std::forward<decltype(handler)>(handler)]() mutable {
                            handler(to_exception_ptr(status), *reply);
                        });
                    });
        };

        return asio::async_initiate<CompletionToken, void(std::exception_ptr, Reply)>(initiation, token, request);
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

    timer.expires_after(std::chrono::milliseconds(500));
    timer.async_wait([&](auto) {
        a_client.greet(
                makeRequest(name, 1000), asio::bind_executor(tp, [&](const std::exception_ptr &, const Reply &reply) {
                    auto tid = current_thread_id();
                    print("Received async response 3 in thread {} ({}): {}\n", tid, tid == tp_tid, reply.greeting());
                }));
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

    run(addr, name);
}
