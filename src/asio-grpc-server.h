#ifndef HELLO_PROTOBUF_ASIO_GRPC_SERVER_H
#define HELLO_PROTOBUF_ASIO_GRPC_SERVER_H

#include "common.h"
#include <asio/experimental/concurrent_channel.hpp>
#include <asio/post.hpp>
#include <grpcpp/server.h>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

#define DEBUG_HANDLER
#ifdef DEBUG_HANDLER
#include <fmt/core.h>
#define DEBUG_PRINT(...) ::fmt::print(__VA_ARGS__)
#else
#define DEBUG_PRINT(...)
#endif

namespace asio_grpc {
#ifdef __cpp_concepts
template<typename T>
concept GrpcReactor = requires(T *t) { t->Finish(grpc::Status{}); };

template<typename T>
concept HasSlot = requires(T *t) { t->slot(); };

#define GRPC_REACTOR ::asio_grpc::GrpcReactor
#define ENABLE_FOR_SLOT(X) std::enable_if_t<HasSlot<X>>
#else
#define GRPC_REACTOR typename
#define ENABLE_FOR_SLOT(X) void
#endif

template<GRPC_REACTOR Reactor>
struct default_reactor_handler {
    using cancellation_slot_type = asio::cancellation_slot;

    default_reactor_handler(Reactor *reactor, cancellation_slot_type slot) : reactor_(reactor), slot_(slot) {}

    template<typename = ENABLE_FOR_SLOT(Reactor)>
    explicit default_reactor_handler(Reactor *reactor) : default_reactor_handler(reactor, reactor->slot()) {}

    [[nodiscard]] cancellation_slot_type get_cancellation_slot() const noexcept { return slot_; }

    void operator()(const std::exception_ptr &eptr, const std::optional<grpc::Status> &status) const {
        try {
            if (eptr) { std::rethrow_exception(eptr); }
            if (status) { reactor_->Finish(*status); }
        } catch (std::exception &e) {
            DEBUG_PRINT("Coroutine exception: {}\n", e.what());
            reactor_->Finish(grpc::Status::CANCELLED);
        } catch (...) {
            DEBUG_PRINT("Catched something not an excpetion!\n");
            reactor_->Finish(grpc::Status::CANCELLED);
        }
    }

    void operator()(const std::error_code &ec, const std::optional<grpc::Status> &status) const {
        (*this)(ec ? std::make_exception_ptr(std::system_error(ec)) : nullptr, status);
    }

private:
    Reactor *reactor_;
    cancellation_slot_type slot_;
};

template<GRPC_REACTOR Reactor, typename Executor = asio::any_io_executor>
struct asio_server_reactor : Reactor {
    using executor_type = Executor;

    explicit asio_server_reactor(executor_type ex) : ex(std::move(ex)) {}

    void OnCancel() final {
        asio::post(ex, [this]() { signal.emit(asio::cancellation_type::terminal); });
    }

    [[nodiscard]] executor_type get_executor() const { return ex; }

    asio::cancellation_slot slot() { return signal.slot(); }

private:
    executor_type ex;
    asio::cancellation_signal signal;
};

struct asio_server_unary_reactor : asio_server_reactor<grpc::ServerUnaryReactor> {
    explicit asio_server_unary_reactor(const asio::any_io_executor &ex)
        : asio_server_reactor<grpc::ServerUnaryReactor>(ex) {}

    void OnDone() final { delete this; }
};

template<typename Reply>
struct asio_server_write_reactor : asio_server_reactor<grpc::ServerWriteReactor<Reply>> {
    using base = asio_server_reactor<grpc::ServerWriteReactor<Reply>>;
    using executor_type = base::executor_type;

    explicit asio_server_write_reactor(const executor_type &ex) : base(ex) {}

    void OnDone() final { delete this; }

    void OnWriteDone(bool ok) final {
        if (ok) {
            send_next();
        } else {
            base::Finish(grpc::Status::CANCELLED);
        }
    }

private:
    virtual void send_next() = 0;
};

struct Server {
    template<typename Executor, typename = asio_grpc::enable_for_executor_t<Executor>>
    Server(Executor ex, std::unique_ptr<grpc::Server> server) : server_(std::move(server)), wait_channel_(ex) {}

    template<typename ExecutionContext, typename = asio_grpc::enable_for_execution_context_t<ExecutionContext>>
    Server(ExecutionContext &ctx, std::unique_ptr<grpc::Server> server)
        : Server(ctx.get_executor(), std::move(server)) {}

    // Wait for the server to shut down. This does not trigger a shutdown, so it can be called after starting
    // the server if no other operation would keep the io_context alive.
    template<typename CompletionToken>
    auto async_wait(CompletionToken &&token) {
        return asio::async_initiate<CompletionToken, void(std::error_code)>(
                [this](auto &&handler) {
                    wait_channel_.async_receive([handler = std::forward<decltype(handler)>(handler)](
                                                        auto ec, bool) mutable { handler(ec); });
                },
                token);
    }

    // Starts shutdown of the server and waits asynchronously. For use without async_wait.
    template<typename CompletionToken>
    auto shutdown(std::chrono::milliseconds timeout, CompletionToken &&token) {
        shutdown(timeout);
        return async_wait(std::forward<CompletionToken>(token));
    }

    // Starts shutdown of the server and waits asynchronously. For use without async_wait.
    template<typename CompletionToken>
    auto shutdown(CompletionToken &&token) {
        shutdown();
        return async_wait(std::forward<CompletionToken>(token));
    }

    // Starts shutdown of the server and returns immediately. For use with async_wait.
    void shutdown(std::chrono::milliseconds timeout) { do_shutdown(timeout); }

    // Starts shutdown of the server and returns immediately. For use with async_wait.
    void shutdown() { do_shutdown(std::nullopt); }

protected:
    template<typename Executor, typename = asio_grpc::enable_for_executor_t<Executor>>
    explicit Server(Executor ex) : server_(nullptr), wait_channel_(ex) {}

    template<typename ExecutionContext, typename = asio_grpc::enable_for_execution_context_t<ExecutionContext>>
    explicit Server(ExecutionContext &ctx) : Server(ctx.get_executor()) {}

    void set_grpc_server(std::unique_ptr<grpc::Server> server) {
        if (!server_) {
            server_ = std::move(server);
        } else {
            throw std::runtime_error("Can't replace gRPC server!\n");
        }
    }

private:
    void do_shutdown(std::optional<std::chrono::milliseconds> timeout) {
        std::thread{[this, timeout]() {
            if (timeout) {
                server_->Shutdown(std::chrono::system_clock::now() + *timeout);
            } else {
                server_->Shutdown();
            }
            server_->Wait();
            wait_channel_.try_send(std::error_code{}, true);
            wait_channel_.close();
        }}.detach();
    }

    std::unique_ptr<grpc::Server> server_;
    asio::experimental::concurrent_channel<void(std::error_code, bool)> wait_channel_;
};
}// namespace asio_grpc

#endif//HELLO_PROTOBUF_ASIO_GRPC_SERVER_H
