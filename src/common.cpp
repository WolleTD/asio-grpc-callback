// common.cpp
//
// SPDX-FileCopyrightText: 2022 Eicke Herbertz
// SPDX-License-Identifier: MIT

#include <sstream>
#include <string>
#include <thread>

std::string current_thread_id() {
    std::stringstream ss;
    ss << std::this_thread::get_id();
    return ss.str();
}
