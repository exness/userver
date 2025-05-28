#pragma once

#include <chrono>
#include <userver/chaotic/openapi/client/middleware.hpp>
#include <userver/chaotic/openapi/client/middleware_factory.hpp>
#include <userver/yaml_config/merge_schemas.hpp>
#include <userver/yaml_config/schema.hpp>

USERVER_NAMESPACE_BEGIN

namespace chaotic::openapi {

class TimeoutRetryMiddleware final : public client::Middleware {
public:
    TimeoutRetryMiddleware(std::chrono::milliseconds timeout, short retries);

    void OnRequest(clients::http::Request& request) override;
    void OnResponse(clients::http::Response&) override;
    void ApplyCommandControl(std::chrono::milliseconds timeout, short retries);

    static std::string GetStaticConfigSchemaStr();

private:
    std::chrono::milliseconds timeout_;
    short retries_;
};

class TimeoutRetryMiddlewareFactory final : public client::MiddlewareFactory {
public:
    std::shared_ptr<client::Middleware> Create(const yaml_config::YamlConfig& config) override;
    std::string GetStaticConfigSchemaStr() override;
};

}  // namespace chaotic::openapi

USERVER_NAMESPACE_END
