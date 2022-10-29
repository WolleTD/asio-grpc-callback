#include "common.h"
#include "hello-client-async.h"
#include "hello-client.h"
#include <asio/bind_executor.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/thread_pool.hpp>
#include <asio/use_future.hpp>
#include <fmt/format.h>
#include <grpcpp/grpcpp.h>
#include <utility>

using fmt::format;
using fmt::print;

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
    print("Starting some async requests!\n");

    auto a_client = HelloClient(ctx, grpc::CreateChannel(addr, grpc::InsecureChannelCredentials()));

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

    struct Reader {
        void operator()(const std::exception_ptr &eptr, const std::optional<StreamReply> &reply) {
            if (!eptr and reply) {
                auto tid = current_thread_id();
                print("Received stream response {} ({}) in thread {} ({}): {}\n", reply->count(), i++, tid,
                      tid == ctx_tid, reply->greeting());
                stream->read_next(std::move(*this));
            } else if (not reply) {
                print("Stream terminated gracefully\n");
            } else {
                try {
                    std::rethrow_exception(eptr);
                } catch (std::runtime_error &e) { print("Stream error: {} i = {}\n", e.what(), i); }
            }
        }

        std::unique_ptr<HelloClient::GreetStream> stream;
        const std::string &ctx_tid;
        int i = 1;
    };

    timer.expires_after(std::chrono::milliseconds(500));
    timer.async_wait([&](auto) {
        a_client.greet(
                makeRequest(name, 1000), asio::bind_executor(tp, [&](const std::exception_ptr &, const Reply &reply) {
                    auto tid = current_thread_id();
                    print("Received async response 3 in thread {} ({}): {}\n", tid, tid == tp_tid, reply.greeting());
                }));
        auto stream = a_client.greet_stream(makeStreamRequest(name, 100, 10));
        auto &stream_ref = *stream;
        stream_ref.read_next(Reader{std::move(stream), ctx_tid});
    });
    ctx.run();
}
