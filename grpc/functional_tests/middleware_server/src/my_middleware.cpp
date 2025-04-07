#include "my_middleware.hpp"

namespace functional_tests {

/// [gRPC CallRequestHook impl example]
void MyMiddleware::PostRecvMessage(ugrpc::server::MiddlewareCallContext&, google::protobuf::Message& request) const {
    auto* message = dynamic_cast<samples::api::GreetingRequest*>(&request);
    auto name = message->name();
    name += " One";
    message->set_name(name);
}

void MyMiddleware::PreSendMessage(ugrpc::server::MiddlewareCallContext&, google::protobuf::Message& response) const {
    auto* message = dynamic_cast<samples::api::GreetingResponse*>(&response);
    auto str = message->greeting();
    str += " EndOne";
    message->set_greeting(str);
}
/// [gRPC CallRequestHook impl example]

}  // namespace functional_tests
