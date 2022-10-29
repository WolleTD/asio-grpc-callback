#ifndef HELLO_PROTOBUF_ASIO_GRPC_SERVER_H
#define HELLO_PROTOBUF_ASIO_GRPC_SERVER_H

#include "common.h"
#include <asio/experimental/concurrent_channel.hpp>
#include <grpcpp/server.h>
#include <optional>
#include <stdexcept>
#include <thread>

namespace asio_grpc {
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
