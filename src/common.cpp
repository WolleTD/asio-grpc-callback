#include <sstream>
#include <string>
#include <thread>

std::string current_thread_id() {
    std::stringstream ss;
    ss << std::this_thread::get_id();
    return ss.str();
}
