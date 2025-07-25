/* THIS FILE IS AUTOGENERATED, DON'T EDIT! */
#include <client/test/client_impl.hpp>

#include <userver/components/component_base.hpp>
#include <userver/components/component_context.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

namespace clients::test {

ClientImpl::ClientImpl(
    const USERVER_NAMESPACE::chaotic::openapi::client::Config& config,
    USERVER_NAMESPACE::clients::http::Client& http_client
)
    : config_(config), http_client_(http_client) {}

USERVER_NAMESPACE::yaml_config::Schema ClientImpl::GetStaticConfigSchema() {
    std::string base_schema = R"(
type: object
description: OpenAPI HTTP client with middlewares
additionalProperties: false
properties:
    base-url:
        type: string
        description: Base URL for the API
    timeout-ms:
        type: integer
        description: Request timeout in milliseconds
        minimum: 1
    attempts:
        type: integer
        description: Maximum number of retry attempts
        minimum: 1
    middlewares:
        type: object
        description: Middleware configurations
        additionalProperties: false
        properties:
)";

    const auto& registry = USERVER_NAMESPACE::chaotic::openapi::client::MiddlewareRegistry::Instance();
    const auto& factories = registry.GetFactories();

    std::string middlewares_yaml = "";
    for (const auto& [name, factory] : factories) {
        auto middleware_schema = factory->GetStaticConfigSchemaStr();
        middlewares_yaml += "            " + name + ":\n";

        std::string indented_yaml = "";
        std::string line;
        std::istringstream schema_stream(middleware_schema);
        while (std::getline(schema_stream, line)) {
            indented_yaml += "                " + line + "\n";
        }
        middlewares_yaml += indented_yaml;
    }

    std::string combined_schema = base_schema + middlewares_yaml;

    return USERVER_NAMESPACE::yaml_config::MergeSchemas<USERVER_NAMESPACE::components::LoggableComponentBase>(
        combined_schema
    );
}

testme_post::Response ClientImpl::TestmePost(
    const testme_post::Request& request,
    const USERVER_NAMESPACE::chaotic::openapi::client::CommandControl& command_control
) {
    auto r = http_client_.CreateRequest();
    r.url(config_.base_url + "/testme");

    SerializeRequest(request, r);

    if (command_control) {
        auto it = middlewares_.find("timeout_retry");
        if (it != middlewares_.end() && (command_control.timeout.count() > 0 || command_control.attempts > 0)) {
            auto timeout_retry =
                std::dynamic_pointer_cast<USERVER_NAMESPACE::chaotic::openapi::TimeoutRetryMiddleware>(it->second);
            if (timeout_retry) {
                timeout_retry->ApplyCommandControl(
                    command_control.timeout.count() > 0 ? command_control.timeout : config_.timeout,
                    command_control.attempts > 0 ? command_control.attempts : config_.attempts
                );
            }
        }

        it = middlewares_.find("follow_redirects");
        if (it != middlewares_.end() && command_control.follow_redirects) {
            auto follow_redirects =
                std::dynamic_pointer_cast<USERVER_NAMESPACE::chaotic::openapi::FollowRedirectsMiddleware>(it->second);
            if (follow_redirects) {
                follow_redirects->ApplyFollowRedirects(*command_control.follow_redirects);
            }
        }
    }

    middleware_manager_.ProcessRequest(r);

    std::shared_ptr<USERVER_NAMESPACE::clients::http::Response> response;
    try {
        response = r.perform();
        middleware_manager_.ProcessResponse(*response);
    } catch (const USERVER_NAMESPACE::clients::http::TimeoutException& e) {
        throw testme_post::TimeoutException();
    }

    return testme_post::ParseResponse(*response);
}

}  // namespace clients::test
