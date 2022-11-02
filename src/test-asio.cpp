#include <asio/awaitable.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/experimental/concurrent_channel.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_future.hpp>
#include <fmt/format.h>
#include <thread>

#include <asio/experimental/co_spawn.hpp>
#include <asio/experimental/coro.hpp>

using asio::any_io_executor;
using asio::awaitable;
using asio::bind_cancellation_slot;
using asio::cancellation_signal;
using asio::cancellation_type;
using asio::co_spawn;
using asio::io_context;
using asio::steady_timer;
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

void coro_exploring() {
    io_context ctx;

    co_spawn(ctx, simple_coro(3),
             [](const std::exception_ptr &, int r) { print("simple_coro: Result is {} (expected 6)\n", r); });

    co_spawn(coro_coro(ctx, 3), [](int r) { print("coro_coro: Result is {} (expected 9)\n", r); });

    co_spawn(ctx, generator_test(), asio::detached);

    auto t = std::thread([work = make_work_guard(ctx)]() {
        auto gen = generator(work.get_executor());
        print("Using generator from thread");
        while (auto i = gen.async_resume(use_future).get()) { print("Generator generated {} for thread\n", *i); }
        print("Generator for thread done\n");
    });

    ctx.run();
    t.join();
}

void cancellation() {
    using namespace std::chrono_literals;

    io_context ctx;
    steady_timer timer{ctx, 100ms};
    steady_timer canceller{ctx, 20ms};
    cancellation_signal sig1, sig2;

    struct my_handler {
        using cancellation_slot_type = asio::cancellation_slot;

        explicit my_handler(cancellation_slot_type slot) : slot_(slot) {}

        [[nodiscard]] cancellation_slot_type get_cancellation_slot() const noexcept { return slot_; }

        void operator()(std::error_code ec) { print("Wait 3 ended with {}\n", ec.message()); }

    private:
        cancellation_slot_type slot_;
    };

    auto my_coro = [&](int id, steady_timer &tim) -> awaitable<void> {
        std::error_code ec;
        co_await tim.async_wait(asio::redirect_error(use_awaitable, ec));
        print("Wait {} ended with {}\n", id, ec.message());
        auto ca = co_await this_coro::cancellation_state;
        print("Cancellation is terminal: {}\n", ca.cancelled() == cancellation_type::terminal);
        print("Cancellation is total: {}\n", ca.cancelled() == cancellation_type::total);
    };

    auto invoking_coro = [&](steady_timer &tim) -> awaitable<void> {
        co_await this_coro::reset_cancellation_state(asio::enable_total_cancellation());
        // Won't cancel as it doesn't inherit the cancellation_state through co_spawn
        co_spawn(ctx, my_coro(4, tim), asio::detached);
        // Will cancel
        co_await my_coro(5, tim);
    };

    // Uncancelled test
    timer.async_wait([](std::error_code ec) { print("Wait 1 ended with {}\n", ec.message()); });

    // This one will be cancelled
    timer.async_wait(bind_cancellation_slot(sig1.slot(),
                                            [](std::error_code ec) { print("Wait 2 ended with {}\n", ec.message()); }));
    // This one should be, but...
    timer.async_wait(my_handler(sig2.slot()));
    // ...this one accidentally uses the same slot, overriding the previous line. Every signal/slot is a 1:1 binding
    co_spawn(ctx, invoking_coro(timer), bind_cancellation_slot(sig2.slot(), asio::detached));

    canceller.async_wait([&](std::error_code ec) {
        sig1.emit(cancellation_type::total);
        sig2.emit(cancellation_type::total);
    });

    ctx.run();
    ctx.restart();

    auto op_coro = []() -> awaitable<void> {
        auto ex = co_await this_coro::executor;
        steady_timer timer1(ex, 100ms), timer2(ex, 50ms);
        std::error_code ec1, ec2;

        using namespace asio::experimental::awaitable_operators;
        auto res = co_await (timer1.async_wait(redirect_error(use_awaitable, ec1)) ||
                             timer2.async_wait(redirect_error(use_awaitable, ec2)));
        print("Completed coro index: {} (expected 1)\nec1: {} ec2: {}\n", res.index(), ec1.message(), ec2.message());
    };

    co_spawn(ctx, op_coro(), asio::detached);
    ctx.run();
}

void channel_check() {
    using namespace std::chrono_literals;
    using channel_t = asio::experimental::concurrent_channel<void(std::error_code, std::optional<int>)>;

    io_context ctx;
    io_context ctx2;
    channel_t channel(ctx);

    auto producer = [](channel_t &channel) -> awaitable<void> {
        try {
            steady_timer timer(co_await this_coro::executor);
            for (int i = 0; i < 10; i++) {
                co_await channel.async_send({}, i, use_awaitable);
                timer.expires_after(10ms);
                co_await timer.async_wait(use_awaitable);
            }
            channel.close();
        } catch (std::exception &e) { print("async_send error: {}\n", e.what()); }
    };

    auto consumer = [](channel_t &channel) -> awaitable<void> {
        try {
            steady_timer timer(co_await this_coro::executor);
            while (auto i = co_await channel.async_receive(use_awaitable)) {
                print("i is {}\n", *i);
                timer.expires_after(12ms);
                co_await timer.async_wait(use_awaitable);
            }
        } catch (std::exception &e) { print("async_receive error: {}\n", e.what()); }
    };

    auto canceller = [](channel_t &channel) -> awaitable<void> {
        steady_timer timer(co_await this_coro::executor, 60ms);
        co_await timer.async_wait(use_awaitable);
        channel.close();
        channel.cancel();
    };

    co_spawn(ctx2, producer(channel), asio::detached);
    co_spawn(ctx, consumer(channel), asio::detached);
    co_spawn(ctx, canceller(channel), asio::detached);
    auto t = std::thread([&]() { ctx2.run(); });
    ctx.run();
    t.join();
}

int main() {
    coro_exploring();
    cancellation();
    channel_check();
}
