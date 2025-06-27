#include <userver/chaotic/openapi/client/middleware_registry.hpp>
#include <userver/chaotic/openapi/middlewares/ssl_middleware.hpp>
#include <userver/clients/http/request.hpp>
#include <userver/components/component_base.hpp>
#include <userver/components/component_config.hpp>
#include <userver/logging/log.hpp>
#include <userver/yaml_config/merge_schemas.hpp>
#include <userver/yaml_config/schema.hpp>

USERVER_NAMESPACE_BEGIN

namespace chaotic::openapi {

SslMiddleware::SslMiddleware(crypto::Certificate&& cert) : cert_(std::move(cert)) {}

void SslMiddleware::OnRequest(clients::http::Request& request) { request.ca(cert_); }

void SslMiddleware::OnResponse(clients::http::Response&) {}

std::string SslMiddleware::GetStaticConfigSchemaStr() {
    return R"(
type: object
description: SSL middleware configuration
additionalProperties: false
properties:
    certificate:
        type: string
        description: SSL certificate content
)";
}

static crypto::Certificate ParseCertificate(const std::string& certificate) {
    if (certificate.empty()) {
        return crypto::Certificate{};
    }

    try {
        return crypto::Certificate::LoadFromString(certificate);
    } catch (const std::exception& ex) {
        LOG_WARNING() << "Failed to parse SSL certificate: " << ex.what() << ". No certificate will be used.";
        return crypto::Certificate{};
    }
}

std::shared_ptr<client::Middleware> SslMiddlewareFactory::Create(
    const USERVER_NAMESPACE::yaml_config::YamlConfig& config
) {
    auto certificate = ParseCertificate(config["certificate"].As<std::string>(""));
    return std::make_shared<SslMiddleware>(std::move(certificate));
}

std::string SslMiddlewareFactory::GetStaticConfigSchemaStr() { return SslMiddleware::GetStaticConfigSchemaStr(); }

namespace {
bool RegisterSslMiddleware() {
    client::MiddlewareRegistry::Instance().Register("ssl", std::make_unique<SslMiddlewareFactory>());
    return true;
}

const bool kSslMiddlewareRegistered = RegisterSslMiddleware();
}  // namespace

}  // namespace chaotic::openapi

USERVER_NAMESPACE_END
