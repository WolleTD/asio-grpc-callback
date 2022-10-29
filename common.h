#include <asio/execution/executor.hpp>
#include <asio/execution_context.hpp>
#include <asio/is_executor.hpp>
#include <string>

namespace asio_grpc {
template<typename Ex>
using enable_for_executor_t = std::enable_if_t<asio::execution::is_executor_v<Ex> || asio::is_executor<Ex>::value>;

template<typename Ctx>
using enable_for_execution_context_t = std::enable_if_t<std::is_convertible_v<Ctx &, asio::execution_context &>>;
}// namespace asio_grpc

std::string current_thread_id();
