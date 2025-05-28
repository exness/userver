#pragma once

#include <userver/chaotic/openapi/client/middleware.hpp>
#include <userver/chaotic/openapi/client/middleware_factory.hpp>
#include <userver/crypto/certificate.hpp>
#include <userver/yaml_config/schema.hpp>

USERVER_NAMESPACE_BEGIN

namespace chaotic::openapi {

class SslMiddleware final : public client::Middleware {
public:
    explicit SslMiddleware(crypto::Certificate&& cert);

    void OnRequest(clients::http::Request& request) override;
    void OnResponse(clients::http::Response&) override;

    static std::string GetStaticConfigSchemaStr();

private:
    crypto::Certificate cert_;
};

class SslMiddlewareFactory final : public client::MiddlewareFactory {
public:
    std::shared_ptr<client::Middleware> Create(const USERVER_NAMESPACE::yaml_config::YamlConfig& config) override;
    std::string GetStaticConfigSchemaStr() override;
};

}  // namespace chaotic::openapi

USERVER_NAMESPACE_END
