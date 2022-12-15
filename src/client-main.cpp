// client-main.cpp
//
// SPDX-FileCopyrightText: 2022 Eicke Herbertz
// SPDX-License-Identifier: MIT

#include <fmt/format.h>

// Declare here to avoid including hello-client.h and linking the protocol definition
void run(const std::string &addr, const std::string &name);

using fmt::print;

int main(int argc, const char *argv[]) {
    if (argc != 3) {
        print(stderr, "usage: {} ADDRESS[:PORT] NAME\n", argv[0]);
        return 1;
    }

    auto addr = std::string(argv[1]);
    auto name = std::string(argv[2]);

    run(addr, name);
}
