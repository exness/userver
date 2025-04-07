#include "my_second_middleware.hpp"

namespace functional_tests {

void MySecondMiddleware::PostRecvMessage(ugrpc::server::MiddlewareCallContext&, google::protobuf::Message& request)
    const {
    auto* message = dynamic_cast<samples::api::GreetingRequest*>(&request);
    auto name = message->name();
    name += " Two";
    message->set_name(name);
}

void MySecondMiddleware::PreSendMessage(ugrpc::server::MiddlewareCallContext&, google::protobuf::Message& response)
    const {
    auto* message = dynamic_cast<samples::api::GreetingResponse*>(&response);
    auto str = message->greeting();
    str += " EndTwo";
    message->set_greeting(str);
}

std::shared_ptr<ugrpc::server::MiddlewareBase>
MySecondMiddlewareComponent::CreateMiddleware(const ugrpc::server::ServiceInfo&, const yaml_config::YamlConfig&) const {
    return middleware_;
}

}  // namespace functional_tests
