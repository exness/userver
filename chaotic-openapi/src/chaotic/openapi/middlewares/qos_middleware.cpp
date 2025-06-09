#include <userver/chaotic/openapi/middlewares/qos_middleware.hpp>

#include <userver/clients/http/request.hpp>
#include <userver/components/component_base.hpp>
#include <userver/components/component_config.hpp>
#include <userver/logging/log.hpp>
#include <userver/yaml_config/merge_schemas.hpp>
#include <userver/yaml_config/schema.hpp>

USERVER_NAMESPACE_BEGIN

namespace chaotic::openapi {

QosMiddleware::QosMiddleware(std::chrono::milliseconds timeout, int retries) : timeout_(timeout), retries_(retries) {}

void QosMiddleware::OnRequest(clients::http::Request& request) {
    request.timeout(timeout_);
    request.retry(retries_);
}

void QosMiddleware::OnResponse(clients::http::Response&) {}

void QosMiddleware::ApplyCommandControl(std::chrono::milliseconds timeout, int retries) {
    timeout_ = timeout;
    retries_ = retries;
}

std::string QosMiddleware::GetStaticConfigSchemaStr() {
    return R"(
type: object
description: Timeout and retry middleware configuration
additionalProperties: false
properties:
    timeout_ms:
        type: integer
        description: Request timeout in milliseconds
        minimum: 1
    retries:
        type: integer
        description: Number of retry attempts
        minimum: 0
)";
}

std::shared_ptr<client::Middleware> QosMiddlewareFactory::Create(const yaml_config::YamlConfig& config) {
    return std::make_shared<QosMiddleware>(
        std::chrono::milliseconds{config["timeout_ms"].As<int>(100)}, config["retries"].As<int>(1)
    );
}

std::string QosMiddlewareFactory::GetStaticConfigSchemaStr() { return QosMiddleware::GetStaticConfigSchemaStr(); }

}  // namespace chaotic::openapi

USERVER_NAMESPACE_END
