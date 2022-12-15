// hello-server.h
//
// SPDX-FileCopyrightText: 2022 Eicke Herbertz
// SPDX-License-Identifier: MIT

#include "asio-grpc-server.h"

struct HelloServer : asio_grpc::Server {
    HelloServer(const asio::any_io_executor &ex, const std::string &address);

    HelloServer(HelloServer &&) noexcept = delete;

    HelloServer &operator=(HelloServer &&) noexcept = delete;

    ~HelloServer();

private:
    std::unique_ptr<grpc::Service> service;
};
