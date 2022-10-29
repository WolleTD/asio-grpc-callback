#include "hello.grpc.pb.h"
#include <string>

void run(const std::string &addr, const std::string &name);

inline hello::Request makeRequest(const std::string &name, int32_t delay_ms) {
    hello::Request request;
    request.set_name(name);
    request.set_delay_ms(delay_ms);
    return request;
}

inline hello::StreamRequest makeStreamRequest(const std::string &name, int32_t delay_ms, int32_t num_replies) {
    hello::StreamRequest request;
    request.mutable_base()->set_name(name);
    request.mutable_base()->set_delay_ms(delay_ms);
    request.set_count(num_replies);
    return request;
}
