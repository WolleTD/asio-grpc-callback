#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/experimental/co_spawn.hpp>
#include <asio/experimental/coro.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <fmt/format.h>
#include <thread>

using asio::any_io_executor;
using asio::awaitable;
using asio::co_spawn;
using asio::io_context;
using asio::use_awaitable;
using asio::use_future;
using asio::experimental::co_spawn;
using asio::experimental::coro;
using fmt::print;
using std::optional;
namespace this_coro = asio::this_coro;

auto simple_coro(int i) -> awaitable<int> { co_return i * 2; }

auto coro_coro(io_context &, int i) -> coro<void() noexcept, int> { co_return i * 3; }

auto generator(any_io_executor) -> coro<int> {
    for (int i = 0; i < 10; i++) { co_yield i; }
}

auto generator_test() -> awaitable<void> {
    auto gen = generator(co_await this_coro::executor);
    while (auto i = co_await gen.async_resume(use_awaitable)) { print("generator genrated: {}\n", *i); }
    print("generator done\n");
}

auto generator_next(coro<int> &generator) -> awaitable<optional<int>> {
    co_return co_await generator.async_resume(use_awaitable);
}

int main() {
    io_context ctx;

    co_spawn(ctx, simple_coro(3),
             [](const std::exception_ptr &, int r) { print("simple_coro: Result is {} (expected 6)\n", r); });

    co_spawn(coro_coro(ctx, 3), [](int r) { print("coro_coro: Result is {} (expected 9)\n", r); });

    co_spawn(ctx, generator_test(), [](const std::exception_ptr &) {});

    auto gen = generator(ctx.get_executor());
    auto t = std::thread([&ctx, &gen, work = make_work_guard(ctx)]() {
        print("Using generator from thread");
        while (auto i = co_spawn(ctx, generator_next(gen), use_future).get()) {
            print("Generator generated {} for thread\n", *i);
        }
        print("Generator for thread done\n");
    });

    ctx.run();
    t.join();
}
