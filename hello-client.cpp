#include "hello.grpc.pb.h"
#include <asio/bind_executor.hpp>
#include <asio/co_spawn.hpp>
#include <asio/experimental/concurrent_channel.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/thread_pool.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>
#include <fmt/format.h>
#include <grpcpp/grpcpp.h>
#include <sstream>
#include <thread>
#include <utility>

#ifdef __cpp_impl_coroutine
using asio::awaitable;
using asio::use_awaitable;
#endif
using fmt::format;
using fmt::print;
using grpc::Channel;
using grpc::ClientContext;
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

            using hT = decltype(handler);
            using dhT = std::decay_t<hT>;
            auto sp_handler = std::make_shared<dhT>(std::forward<hT>(handler));

            stub_->async()->greet(ctx.get(), &request, reply.get(),
                                  [reply, ctx, work, sp_handler](const grpc::Status &status) mutable {
                                      print("gRPC handler is running in thread {}\n", current_thread_id());
                                      auto ex = asio::get_associated_executor(*sp_handler, work.get_executor());
                                      asio::post(ex, [status, reply, sp_handler]() {
                                          (*sp_handler)(to_exception_ptr(status), *reply);
                                      });
                                  });
        };

        return asio::async_initiate<CompletionToken, void(std::exception_ptr, Reply)>(initiation, token, request);
    }

    // asio is much lower level and doesn't have such thing as a stream. In C++20, we can probably reflect this onto co_yield,
    // but this example shall not use coroutines. Even though it's really hard.
    // So, to get into solution space: the user knows he's calling a protobuf stream method [citation needed], so we can invent
    // our own API. The gRPC API requires us to build an object anyway, so we could use a public object with its own read()
    // method – fortunately, asio 1.24 has experimental channels, so lets try those!
    struct GreetStream;

    std::unique_ptr<GreetStream> greet_stream(const StreamRequest &request) {
        return std::make_unique<GreetStream>(this, request);
    }

    struct GreetStream {
        using channel_t = asio::experimental::concurrent_channel<void(std::exception_ptr, StreamReply)>;

        GreetStream(AsyncHelloClient *self, const StreamRequest &request)
            : channel_(self->ctx_), reader_(self->stub_.get(), request, channel_) {}

        template<typename CompletionToken>
        auto read_next(CompletionToken &&token) {
            return channel_.async_receive(std::forward<CompletionToken>(token));
        }

        struct GreetReader : public grpc::ClientReadReactor<StreamReply> {
            GreetReader(AsyncHello::Stub *stub, const StreamRequest &request, channel_t &channel) : channel_(channel) {
                stub->async()->greet_stream(&ctx_, &request, this);
                StartRead(&reply_);
                StartCall();
            }

            void OnReadDone(bool ok) override {
                if (ok) {
                    channel_.async_send({}, reply_, asio::use_future).get();
                    StartRead(&reply_);
                }
            }

            void OnDone(const grpc::Status &status) override {
                if (!status.ok()) { channel_.async_send(to_exception_ptr(status), reply_, asio::use_future).get(); }
                channel_.close();
            }

        private:
            ClientContext ctx_;
            StreamReply reply_;
            channel_t &channel_;
        };

    private:
        channel_t channel_;
        GreetReader reader_;
    };

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

StreamRequest makeStreamRequest(const std::string &name, int32_t delay_ms, int32_t num_replies) {
    StreamRequest request;
    request.mutable_base()->set_name(name);
    request.mutable_base()->set_delay_ms(delay_ms);
    request.set_count(num_replies);
    return request;
}

#ifdef __cpp_impl_coroutine
void run_cpp20(const std::string &addr, const std::string &name) {
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

    auto error_handler = [](const std::exception_ptr &eptr) {
        try {
            if (eptr) { std::rethrow_exception(eptr); }
        } catch (std::exception &e) { print(stderr, "Coroutine ended with exception: {}\n", e.what()); }
    };

    auto coro1 = [&]() -> awaitable<void> {
        auto request = makeRequest(name, 1000);
        auto reply = co_await a_client.greet(request, use_awaitable);
        auto tid = current_thread_id();
        print("Received async response 1 in thread {} ({}): {}\n", tid, tid == tp_tid, reply.greeting());
    };

    auto coro2 = [&]() -> awaitable<void> {
        auto request = makeRequest(name, 2000);
        auto reply = co_await a_client.greet(request, use_awaitable);
        auto tid = current_thread_id();
        print("Received async response 2 in thread {} ({}): {}\n", tid, tid == ctx_tid, reply.greeting());
        print("Restarting timer!\n");

        // This will run last!
        timer.expires_after(std::chrono::milliseconds(1000));
        co_await timer.async_wait(use_awaitable);
        print("Timer started in response 2 expired. Reply was: {}\n", reply.greeting());
    };

    auto coro3 = [&]() -> awaitable<void> {
        timer.expires_after(std::chrono::milliseconds(500));
        co_await timer.async_wait(use_awaitable);
        auto coro3_1 = [&]() -> awaitable<void> {
            auto request = makeRequest(name, 1000);
            auto reply = co_await a_client.greet(request, use_awaitable);
            auto tid = current_thread_id();
            print("Received async response 3 in thread {} ({}): {}\n", tid, tid == tp_tid, reply.greeting());
        };
        co_spawn(tp, coro3_1, error_handler);
        auto stream = a_client.greet_stream(makeStreamRequest(name, 100, 10));
        int i = 1;
        for (;;) {
            auto sreply = co_await stream->read_next(use_awaitable);
            auto tid = current_thread_id();
            print("Received stream response {} ({}) in thread {} ({}): {}\n", sreply.count(), i++, tid, tid == ctx_tid,
                  sreply.greeting());
        }
    };

    co_spawn(tp, coro1, error_handler);
    co_spawn(ctx, coro2, error_handler);
    co_spawn(ctx, coro3, error_handler);

    ctx.run();
}
#endif

void run_cpp17(const std::string &addr, const std::string &name) {
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

    struct Reader {
        void operator()(const std::exception_ptr &eptr, const StreamReply &reply) {
            if (!eptr) {
                auto tid = current_thread_id();
                print("Received stream response {} ({}) in thread {} ({}): {}\n", reply.count(), i++, tid,
                      tid == ctx_tid, reply.greeting());
                stream->read_next(std::move(*this));
            } else {
                try {
                    std::rethrow_exception(eptr);
                } catch (std::runtime_error &e) { print("Stream error: {} i = {}\n", e.what(), i); }
            }
        }

        std::unique_ptr<AsyncHelloClient::GreetStream> stream;
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

int main(int argc, const char *argv[]) {
    if (argc != 3) {
        print(stderr, "usage: {} ADDRESS[:PORT] NAME\n", argv[0]);
        return 1;
    }

    auto addr = std::string(argv[1]);
    auto name = std::string(argv[2]);

    run_cpp17(addr, name);
#ifdef __cpp_impl_coroutine
    run_cpp20(addr, name);
#endif
}
