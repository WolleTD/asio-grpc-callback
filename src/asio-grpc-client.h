#ifndef HELLO_PROTOBUF_ASIO_GRPC_CLIENT_H
#define HELLO_PROTOBUF_ASIO_GRPC_CLIENT_H

#include <asio/bind_cancellation_slot.hpp>
#include <asio/experimental/concurrent_channel.hpp>
#include <asio/post.hpp>
#include <asio/redirect_error.hpp>
#include <asio/use_future.hpp>
#include <grpcpp/impl/codegen/client_callback.h>
#include <grpcpp/impl/codegen/client_context.h>
#include <grpcpp/impl/codegen/status.h>
#include <optional>
#include <stdexcept>

#define DEBUG_HANDLER
#ifdef DEBUG_HANDLER
#include <fmt/core.h>
#define DEBUG_PRINT(...) ::fmt::print(__VA_ARGS__)
#else
#define DEBUG_PRINT(...)
#endif

namespace asio_grpc {
class grpc_error : public std::runtime_error {
public:
    explicit grpc_error(const grpc::Status &status)
        : std::runtime_error(status.error_message()), code_(status.error_code()) {}

    [[nodiscard]] grpc::StatusCode error_code() const { return code_; }

private:
    grpc::StatusCode code_;
};

inline std::exception_ptr to_exception_ptr(const grpc::Status &status) {
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
concept GrpcStreamCompletionToken =
        ::asio::completion_token_for<T, void(std::exception_ptr, const std::optional<Reply> &)>;

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

template<typename Ex>
using enable_for_executor_t = std::enable_if_t<asio::execution::is_executor_v<Ex> || asio::is_executor<Ex>::value>;

template<typename Ctx>
using enable_for_execution_context_t = std::enable_if_t<std::is_convertible_v<Ctx &, asio::execution_context &>>;

template<typename Reply, typename Ex, GRPC_COMPLETION_TOKEN(Reply) CompletionToken, GRPC_CALL(Reply) Call,
         typename = enable_for_executor_t<Ex>>
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
        auto cs = asio::get_associated_cancellation_slot(*sp_handler);
        if (cs.is_connected()) {
            cs.assign([ctx](asio::cancellation_type_t type) {
                if (type != asio::cancellation_type::none) { ctx->TryCancel(); }
            });
        }

        call(ctx.get(), reply.get(), [reply, ctx, work, sp_handler](const grpc::Status &status) {
            auto ex = asio::get_associated_executor(*sp_handler, work.get_executor());
            auto cs = asio::get_associated_cancellation_slot(*sp_handler);
            asio::post(ex, asio::bind_cancellation_slot(cs, [status, reply, sp_handler]() {
                           (*sp_handler)(to_exception_ptr(status), *reply);
                       }));
        });
    };

    return asio::async_initiate<CompletionToken, void(std::exception_ptr, Reply)>(initiation, token,
                                                                                  std::forward<Call>(call));
}

template<typename Reply, typename Ctx, GRPC_COMPLETION_TOKEN(Reply) CompletionToken, GRPC_CALL(Reply) Call,
         typename = enable_for_execution_context_t<Ctx>>
auto async_initiate_grpc(Ctx &ctx, CompletionToken &&token, Call &&call) {
    return async_initiate_grpc<Reply>(ctx.get_executor(), std::forward<CompletionToken>(token),
                                      std::forward<Call>(call));
}

template<typename Reply>
struct StreamChannel {
    using channel_t = asio::experimental::concurrent_channel<void(std::exception_ptr, std::optional<Reply>)>;

    template<typename Ex, GRPC_STREAM_CALL(Reply) RequestHandler, typename = enable_for_executor_t<Ex>>
    StreamChannel(Ex ex, RequestHandler &&request)
        : channel_(std::make_shared<channel_t>(ex)), reader_(std::make_shared<StreamReader>(std::forward<RequestHandler>(request), channel_)) {
        reader_->set_self(reader_);
        reader_->StartCall();
    }

    template<typename Ctx, GRPC_STREAM_CALL(Reply) RequestHandler, typename = enable_for_execution_context_t<Ctx>>
    StreamChannel(Ctx &ctx, RequestHandler &&request)
        : StreamChannel(ctx.get_executor(), std::forward<RequestHandler>(request)) {}

    StreamChannel(const StreamChannel &) = delete;
    StreamChannel& operator=(const StreamChannel &) = delete;
    StreamChannel(StreamChannel &&) noexcept = default;
    StreamChannel& operator=(StreamChannel &&) noexcept = default;

    ~StreamChannel() {
        if (reader_) reader_->Cancel(true);
    }

    template<GRPC_STREAM_COMPLETION_TOKEN(Reply) CompletionToken>
    auto read_next(CompletionToken &&token) {
        return channel_->async_receive(std::forward<CompletionToken>(token));
    }

    void cancel() {
        reader_->Cancel(false);
    }

    struct StreamReader : public grpc::ClientReadReactor<Reply> {
        using base = grpc::ClientReadReactor<Reply>;

        template<GRPC_STREAM_CALL(Reply) RequestHandler>
        StreamReader(RequestHandler &&request, const std::shared_ptr<channel_t> &channel) : channel_(channel) {
            request(&ctx_, this);
            base::StartRead(&reply_);
        }

        void set_self(const std::shared_ptr<StreamReader> &self) {
            self_ = self;
        }

        void OnReadDone(bool ok) override {
            if (ok && channel_->is_open()) {
                std::error_code ec;
                channel_->async_send({}, reply_, redirect_error(asio::use_future, ec)).get();
                if (ec) DEBUG_PRINT("async_send error: {}\n", ec.message());
                base::StartRead(&reply_);
            }
        }

        void OnDone(const grpc::Status &status) override {
            auto self = std::exchange(self_, nullptr);
            if (channel_->is_open()) {
                std::error_code ec;
                channel_->async_send(to_exception_ptr(status), std::nullopt, redirect_error(asio::use_future, ec)).get();
                if (ec) DEBUG_PRINT("async_send error: {}\n", ec.message());
                channel_->close();
            }
        }

        void Cancel(bool destructed) {
            if (self_) ctx_.TryCancel();
            if (destructed) {
                channel_->close();
                channel_->cancel();
            }
        }

    private:
        grpc::ClientContext ctx_;
        Reply reply_;
        std::shared_ptr<channel_t> channel_;
        std::shared_ptr<StreamReader> self_;
    };

private:
    std::shared_ptr<channel_t> channel_;
    std::shared_ptr<StreamReader> reader_;
};
}// namespace asio_grpc

#endif//HELLO_PROTOBUF_ASIO_GRPC_CLIENT_H
