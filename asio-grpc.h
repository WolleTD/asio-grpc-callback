#ifndef HELLO_PROTOBUF_ASIO_GRPC_H
#define HELLO_PROTOBUF_ASIO_GRPC_H

#include <asio/experimental/concurrent_channel.hpp>
#include <asio/post.hpp>
#include <asio/use_future.hpp>
#include <grpcpp/impl/codegen/client_callback.h>
#include <grpcpp/impl/codegen/client_context.h>
#include <grpcpp/impl/codegen/status.h>
#include <stdexcept>

namespace asio_grpc {
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

#ifdef __cpp_concepts
template<typename T, typename Reply>
concept GrpcCall = ::asio::completion_handler_for<T, void(grpc::ClientContext *, Reply *, void(grpc::Status))>;

template<typename T, typename Reply>
concept GrpcCompletionToken = ::asio::completion_token_for<T, void(std::exception_ptr, const Reply &)>;

template<typename T, typename Reply>
concept GrpcStreamCall =
        ::asio::completion_handler_for<T, void(grpc::ClientContext *, grpc::ClientReadReactor<Reply> *)>;

template<typename T, typename Reply>
concept GrpcStreamCompletionToken = ::asio::completion_token_for<T, void(std::exception_ptr, const Reply &)>;

#define GRPC_CALL(X) ::asio_grpc::GrpcCall<X>
#define GRPC_COMPLETION_TOKEN(X) ::asio_grpc::GrpcCompletionToken<X>
#define GRPC_STREAM_CALL(X) ::asio_grpc::GrpcStreamCall<X>
#define GRPC_STREAM_COMPLETION_TOKEN(X) ::asio_grpc::GrpcStreamCompletionToken<X>
#else
#define GRPC_CALL(X) typename
#define GRPC_COMPLETION_TOKEN(X) typename
#define GRPC_STREAM_CALL(X) typename
#define GRPC_STREAM_COMPLETION_TOKEN(X) typename
#endif

template<typename Reply, typename Ex, GRPC_COMPLETION_TOKEN(Reply) CompletionToken, GRPC_CALL(Reply) Call,
         typename = std::enable_if_t<asio::execution::is_executor_v<Ex> || asio::is_executor<Ex>::value>>
auto async_initiate_grpc(Ex ex, CompletionToken &&token, Call &&call) {
    auto initiation = [ex](auto &&handler, auto &&call) {
        // Keeps the io_context active, we are basically an io-object for asio
        auto work = asio::make_work_guard(ex);
        // These both need to live through the async call
        auto reply = std::make_shared<Reply>();
        auto ctx = std::make_shared<grpc::ClientContext>();

        // gRPC uses std::function somewhere, but the use_awaitable and use_future handlers are not
        // copy-constructible, so we wrap the handler in a shared_ptr as well.
        using hT = decltype(handler);
        using dhT = std::decay_t<hT>;
        auto sp_handler = std::make_shared<dhT>(std::forward<hT>(handler));

        call(ctx.get(), reply.get(), [reply, ctx, work, sp_handler](const grpc::Status &status) {
            auto ex = asio::get_associated_executor(*sp_handler, work.get_executor());
            asio::post(ex, [status, reply, sp_handler]() { (*sp_handler)(to_exception_ptr(status), *reply); });
        });
    };

    return asio::async_initiate<CompletionToken, void(std::exception_ptr, Reply)>(initiation, token,
                                                                                  std::forward<Call>(call));
}

template<typename Reply, typename Ctx, GRPC_COMPLETION_TOKEN(Reply) CompletionToken, GRPC_CALL(Reply) Call,
         typename = std::enable_if_t<std::is_convertible_v<Ctx &, asio::execution_context &>>>
auto async_initiate_grpc(Ctx &ctx, CompletionToken &&token, Call &&call) {
    return async_initiate_grpc<Reply>(ctx.get_executor(), std::forward<CompletionToken>(token),
                                      std::forward<Call>(call));
}

template<typename Reply>
struct StreamChannel {
    using channel_t = asio::experimental::concurrent_channel<void(std::exception_ptr, Reply)>;

    template<typename Ex, GRPC_STREAM_CALL(Reply) RequestHandler,
             typename = std::enable_if_t<asio::execution::is_executor_v<Ex> || asio::is_executor<Ex>::value>>
    StreamChannel(Ex ex, RequestHandler &&request)
        : channel_(ex), reader_(std::forward<RequestHandler>(request), channel_) {
        reader_.StartCall();
    }

    template<typename Ctx, GRPC_STREAM_CALL(Reply) RequestHandler,
             typename = std::enable_if_t<std::is_convertible_v<Ctx &, asio::execution_context &>>>
    StreamChannel(Ctx &ctx, RequestHandler &&request)
        : StreamChannel(ctx.get_executor(), std::forward<RequestHandler>(request)) {}

    template<GRPC_STREAM_COMPLETION_TOKEN(Reply) CompletionToken>
    auto read_next(CompletionToken &&token) {
        return channel_.async_receive(std::forward<CompletionToken>(token));
    }

    struct StreamReader : public grpc::ClientReadReactor<Reply> {
        using base = grpc::ClientReadReactor<Reply>;

        template<GRPC_STREAM_CALL(Reply) RequestHandler>
        StreamReader(RequestHandler &&request, channel_t &channel) : channel_(channel) {
            request(&ctx_, this);
            base::StartRead(&reply_);
        }

        void OnReadDone(bool ok) override {
            if (ok) {
                channel_.async_send({}, reply_, asio::use_future).get();
                base::StartRead(&reply_);
            }
        }

        void OnDone(const grpc::Status &status) override {
            if (!status.ok()) { channel_.async_send(to_exception_ptr(status), reply_, asio::use_future).get(); }
            channel_.close();
        }

    private:
        grpc::ClientContext ctx_;
        Reply reply_;
        channel_t &channel_;
    };

private:
    channel_t channel_;
    StreamReader reader_;
};
}// namespace asio_grpc

#endif//HELLO_PROTOBUF_ASIO_GRPC_H
