#pragma once

#include <userver/utest/using_namespace_userver.hpp>

#include <userver/components/component.hpp>
#include <userver/components/component_base.hpp>
#include <userver/engine/async.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

#include <userver/ugrpc/client/client_factory_component.hpp>
#include <userver/ugrpc/client/impl/client_data.hpp>
#include <userver/ugrpc/client/simple_client_component.hpp>

#include <samples/greeter_client.usrv.pb.hpp>

namespace samples {

using Client = api::GreeterServiceClient;

// It is used only to test the count of dedicated channels
using GreeterClientComponent = ugrpc::client::SimpleClientComponent<Client>;

class GreeterClient final : public components::ComponentBase {
public:
    static constexpr std::string_view kName = "greeter-client";

    GreeterClient(const components::ComponentConfig& config, const components::ComponentContext& context)
        : ComponentBase(config, context),
          client_factory_(context.FindComponent<ugrpc::client::ClientFactoryComponent>().GetFactory()),
          client_(client_factory_.MakeClient<Client>("greeter", config["endpoint"].As<std::string>())) {
        // Tests dedicated-channel-count from SimpleClientComponent
        auto& client =
            context.FindComponent<ugrpc::client::SimpleClientComponent<Client>>("greeter-client-component").GetClient();
        const auto& data = ugrpc::client::impl::GetClientData(client);
        const auto stub_state = data.GetStubState();
        UASSERT(stub_state->dedicated_stubs[0].Size() == 3);
        UASSERT(stub_state->dedicated_stubs[1].Size() == 0);
        UASSERT(stub_state->dedicated_stubs[2].Size() == 2);
        UASSERT(stub_state->dedicated_stubs[3].Size() == 0);
        UASSERT(stub_state->dedicated_stubs[4].Size() == 0);
    }

    inline std::string SayHello(std::string name, bool is_small_timeout);

    inline std::string SayHelloResponseStream(std::string name, bool is_small_timeout);

    inline std::string SayHelloRequestStream(const std::vector<std::string>& names, bool is_small_timeout);

    inline std::string SayHelloStreams(const std::vector<std::string>& names, bool is_small_timeout);

    inline std::string SayHelloIndependentStreams(const std::vector<std::string>& names, bool is_small_timeout);

    inline static yaml_config::Schema GetStaticConfigSchema();

private:
    inline ugrpc::client::CallOptions CreateCallOptions(bool is_small_timeout);

    ugrpc::client::ClientFactory& client_factory_;
    Client client_;
};

yaml_config::Schema GreeterClient::GetStaticConfigSchema() {
    return yaml_config::MergeSchemas<components::ComponentBase>(R"(
type: object
description: >
    a user-defined wrapper around api::GreeterServiceClient that provides
    a simplified interface.
additionalProperties: false
properties:
    endpoint:
        type: string
        description: >
            the service endpoint (URI). We talk to our own service,
            which is kind of pointless, but works for an example
)");
}

ugrpc::client::CallOptions GreeterClient::CreateCallOptions(bool is_small_timeout) {
    ugrpc::client::CallOptions call_options;
    call_options.SetTimeout(std::chrono::seconds{is_small_timeout ? 1 : 20});
    call_options.SetClientContextFactory([] {
        auto client_context = std::make_unique<grpc::ClientContext>();
        client_context->set_wait_for_ready(true);
        return client_context;
    });
    return call_options;
}

std::string GreeterClient::SayHello(std::string name, bool is_small_timeout) {
    api::GreetingRequest request;
    request.set_name(std::move(name));

    api::GreetingResponse response = client_.SayHello(request, CreateCallOptions(is_small_timeout));

    return std::move(*response.mutable_greeting());
}

std::string GreeterClient::SayHelloResponseStream(std::string name, bool is_small_timeout) {
    std::string result{};
    api::GreetingRequest request;
    request.set_name(std::move(name));

    auto stream = client_.SayHelloResponseStream(request, CreateCallOptions(is_small_timeout));
    api::GreetingResponse response;
    while (stream.Read(response)) {
        result.append(response.greeting());
        result.append("\n");
    }
    return result;
}

std::string GreeterClient::SayHelloRequestStream(const std::vector<std::string>& names, bool is_small_timeout) {
    const std::string result{};
    auto stream = client_.SayHelloRequestStream(CreateCallOptions(is_small_timeout));
    for (const auto& name : names) {
        api::GreetingRequest request;
        request.set_name(grpc::string(name));
        if (!stream.Write(request)) {
            return "Error write";
        }
    }
    auto response = stream.Finish();
    return std::move(*response.mutable_greeting());
}

std::string GreeterClient::SayHelloStreams(const std::vector<std::string>& names, bool is_small_timeout) {
    std::string result{};
    auto stream = client_.SayHelloStreams(CreateCallOptions(is_small_timeout));
    for (const auto& name : names) {
        api::GreetingRequest request;
        request.set_name(grpc::string(name));
        stream.WriteAndCheck(request);
        api::GreetingResponse response;
        if (stream.Read(response)) {
            result.append(std::move(*response.mutable_greeting()));
            result.append("\n");
        }
    }
    return result;
}

std::string GreeterClient::SayHelloIndependentStreams(const std::vector<std::string>& names, bool is_small_timeout) {
    std::string result{};
    auto stream = client_.SayHelloIndependentStreams(CreateCallOptions(is_small_timeout));
    auto write_task = engine::AsyncNoSpan([&stream, &names] {
        for (const auto& name : names) {
            api::GreetingRequest request;
            request.set_name(grpc::string(name));
            if (!stream.Write(request)) {
                throw std::runtime_error("Write error");
            }
        }
        const bool is_success = stream.WritesDone();
        LOG_DEBUG() << "Write task finish: " << is_success;
    });

    auto read_task = engine::AsyncNoSpan([&stream, &result] {
        api::GreetingResponse response;
        while (stream.Read(response)) {
            result.append(std::move(*response.mutable_greeting()));
            result.append("\n");
        }
    });
    write_task.Get();
    read_task.Get();
    return result;
}

}  // namespace samples
