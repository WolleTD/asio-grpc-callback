#include "common.h"
#include "hello-server.h"
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

#ifdef __cpp_impl_coroutine
awaitable<void> run_server(std::string addr) {
    auto ex = co_await asio::this_coro::executor;
    auto server = HelloServer(ex, addr);
    print("Waiting for clients...\n");

    auto sig = asio::signal_set(ex, SIGINT, SIGTERM);
    co_await sig.async_wait(use_awaitable);
    print("Shutting down server\n");
    co_await server.shutdown(use_awaitable);
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
