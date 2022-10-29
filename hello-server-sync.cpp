#include "asio-grpc.h"
#include "common.h"
#include "hello.grpc.pb.h"
#include <asio/awaitable.hpp>
#ifdef __cpp_impl_coroutine
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#endif
#include <asio/io_context.hpp>
#include <asio/signal_set.hpp>
#include <fmt/format.h>
#include <grpcpp/grpcpp.h>

#ifdef __cpp_impl_coroutine
using asio::awaitable;
using asio::use_awaitable;
#endif
using fmt::format;
using fmt::print;
using grpc::ServerContext;
using hello::Hello;
using hello::Reply;
using hello::Request;
using hello::StreamReply;
using hello::StreamRequest;

class HelloServiceImpl final : public Hello::Service {
    grpc::Status greet(ServerContext *ctx, const Request *request, Reply *reply) override {
        print("Server reacting in Thread {}\n", current_thread_id());
        auto msg = format("Hello {}!", request->name());
        reply->set_greeting(msg);

        return grpc::Status::OK;
    }
};

struct HelloServer : asio_grpc::Server {
    template<typename Executor, typename = asio_grpc::enable_for_executor_t<Executor>>
    HelloServer(Executor ex, const std::string &address) : asio_grpc::Server(ex) {
        grpc::EnableDefaultHealthCheckService(true);
        auto builder = grpc::ServerBuilder();

        builder.AddListeningPort(address, grpc::InsecureServerCredentials());
        builder.RegisterService(&sync_service);

        set_grpc_server(builder.BuildAndStart());
    }

    HelloServer(HelloServer &&) noexcept = delete;
    HelloServer &operator=(HelloServer &&) noexcept = delete;

private:
    HelloServiceImpl sync_service;
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
