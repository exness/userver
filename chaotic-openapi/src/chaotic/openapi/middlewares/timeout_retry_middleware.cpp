#include <userver/chaotic/openapi/client/middleware_registry.hpp>
#include <userver/chaotic/openapi/middlewares/timeout_retry_middleware.hpp>
#include <userver/clients/http/request.hpp>
#include <userver/components/component_base.hpp>
#include <userver/components/component_config.hpp>
#include <userver/logging/log.hpp>
#include <userver/yaml_config/merge_schemas.hpp>
#include <userver/yaml_config/schema.hpp>

USERVER_NAMESPACE_BEGIN

namespace chaotic::openapi {

TimeoutRetryMiddleware::TimeoutRetryMiddleware(std::chrono::milliseconds timeout, short retries)
    : timeout_(timeout), retries_(retries) {}

void TimeoutRetryMiddleware::OnRequest(clients::http::Request& request) {
    request.timeout(timeout_);
    request.retry(retries_);
}

void TimeoutRetryMiddleware::OnResponse(clients::http::Response&) {}

void TimeoutRetryMiddleware::ApplyCommandControl(std::chrono::milliseconds timeout, short retries) {
    timeout_ = timeout;
    retries_ = retries;
}

std::string TimeoutRetryMiddleware::GetStaticConfigSchemaStr() {
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

std::shared_ptr<client::Middleware> TimeoutRetryMiddlewareFactory::Create(const yaml_config::YamlConfig& config) {
    return std::make_shared<TimeoutRetryMiddleware>(
        std::chrono::milliseconds{config["timeout_ms"].As<int>(100)}, config["retries"].As<short>(1)
    );
}

std::string TimeoutRetryMiddlewareFactory::GetStaticConfigSchemaStr() {
    return TimeoutRetryMiddleware::GetStaticConfigSchemaStr();
}

namespace {
bool RegisterTimeoutRetryMiddleware() {
    client::MiddlewareRegistry::Instance().Register("timeout_retry", std::make_unique<TimeoutRetryMiddlewareFactory>());
    return true;
}

const bool kTimeoutRetryRegistered = RegisterTimeoutRetryMiddleware();
}  // namespace

}  // namespace chaotic::openapi

USERVER_NAMESPACE_END
