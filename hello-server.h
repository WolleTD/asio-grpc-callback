#include "asio-grpc.h"

struct HelloServer : asio_grpc::Server {
    HelloServer(const asio::any_io_executor &ex, const std::string &address);

    HelloServer(HelloServer &&) noexcept = delete;

    HelloServer &operator=(HelloServer &&) noexcept = delete;

    ~HelloServer();

private:
    std::unique_ptr<grpc::Service> service;
};
