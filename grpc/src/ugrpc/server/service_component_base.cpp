#include <string>
#include <string_view>
#include <userver/ugrpc/server/service_component_base.hpp>

#include <userver/components/component_config.hpp>
#include <userver/components/component_context.hpp>
#include <userver/formats/common/merge.hpp>
#include <userver/formats/yaml/value_builder.hpp>
#include <userver/middlewares/runner.hpp>
#include <userver/utils/assert.hpp>
#include <userver/yaml_config/merge_schemas.hpp>
#include <userver/yaml_config/yaml_config.hpp>

#include <ugrpc/server/impl/parse_config.hpp>
#include <userver/ugrpc/server/middlewares/pipeline.hpp>
#include <userver/ugrpc/server/server_component.hpp>
#include <userver/ugrpc/server/service_base.hpp>

USERVER_NAMESPACE_BEGIN

namespace {

std::string GetServerName(const components::ComponentConfig& config) {
    static constexpr std::string_view kServerNameKey = "server-name";
    if (config.HasMember(kServerNameKey)) {
        return config[kServerNameKey].As<std::string>();
    }
    return "grpc-server";
}

}  // namespace

namespace ugrpc::server {
ServiceComponentBase::ServiceComponentBase(
    const components::ComponentConfig& config,
    const components::ComponentContext& context
)
    : impl::MiddlewareRunnerComponentBase(config, context, MiddlewarePipelineComponent::kName),
      server_(context.FindComponent<ServerComponent>(GetServerName(config))),
      config_(server_.ParseServiceConfig(config, context)),
      info_(ServiceInfo{config.Name()}) {
    config_.middlewares = CreateMiddlewares(*info_);
}

ServiceComponentBase::~ServiceComponentBase() = default;

void ServiceComponentBase::RegisterService(ServiceBase& service) {
    UINVARIANT(!registered_.exchange(true), "Register must only be called once");

    server_.GetServer().AddService(service, std::move(config_));
}

void ServiceComponentBase::RegisterService(GenericServiceBase& service) {
    UINVARIANT(!registered_.exchange(true), "Register must only be called once");
    server_.GetServer().AddService(service, std::move(config_));
}

yaml_config::Schema ServiceComponentBase::GetStaticConfigSchema() {
    return yaml_config::MergeSchemas<impl::MiddlewareRunnerComponentBase>(R"(
type: object
description: base class for all the gRPC service components
additionalProperties: false
properties:
    task-processor:
        type: string
        description: the task processor to use for responses
        defaultDescription: uses grpc-server.service-defaults.task-processor
    server-name:
        type: string
        description: the name of the server to use
        defaultDescription: grpc-server
)");
}

}  // namespace ugrpc::server

USERVER_NAMESPACE_END
