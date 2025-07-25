#include <userver/server/handlers/implicit_options.hpp>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <server/handlers/auth/auth_checker.hpp>
#include <server/http/handler_info_index.hpp>
#include <server/http/handler_methods.hpp>
#include <server/http/http_request_handler.hpp>
#include <server/middlewares/auth.hpp>
#include <userver/components/component.hpp>
#include <userver/http/common_headers.hpp>
#include <userver/server/component.hpp>
#include <userver/server/handlers/auth/auth_checker_factory.hpp>
#include <userver/server/handlers/auth/auth_checker_settings_component.hpp>
#include <userver/server/handlers/auth/handler_auth_config.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

USERVER_NAMESPACE_BEGIN

namespace server::handlers {
namespace {

using AuthCheckers = std::unordered_map<std::string, auth::AuthCheckerBasePtr>;

AuthCheckers MakeAuthCheckers(const components::ComponentConfig& config, const components::ComponentContext& context) {
    const auto auth_config_raw = config["auth_checkers"];
    if (auth_config_raw.IsMissing()) return {};

    const auth::HandlerAuthConfig auth_config(auth_config_raw);

    const auto* auth_middleware_factory = context.FindComponentOptional<server::middlewares::AuthFactory>();
    if (!auth_middleware_factory) return {};

    AuthCheckers checkers;
    for (const auto& type : auth_config.GetTypes()) {
        try {
            const auto& auth_factory = auth_middleware_factory->GetAuthCheckerFactory(type);
            auto sp_checker = auth_factory.MakeAuthChecker(auth_config);
            if (sp_checker) {
                checkers[type] = sp_checker;
                LOG_INFO() << "Loaded " << type << " auth checker for implicit options handler";
            } else
                LOG_ERROR() << "Internal error during creating " << type << " auth checker";
        } catch (const std::exception& err) {
            LOG_ERROR() << "Unable to create " << type << " auth checker "
                        << "for implicit OPTIONS handler, skipping the check: " << err.what();
        }
    }

    return checkers;
}

}  // namespace

ImplicitOptions::ImplicitOptions(
    const components::ComponentConfig& config,
    const components::ComponentContext& context,
    bool is_monitor
)
    : HttpHandlerBase(config, context, is_monitor),
      server_(context.FindComponent<components::Server>().GetServer()),
      auth_checkers_(MakeAuthCheckers(config, context)) {}

ImplicitOptions::~ImplicitOptions() = default;

std::string ImplicitOptions::ExtractAllowedMethods(const std::string& path) const {
    std::vector<std::string> allowed_methods = {ToString(http::HttpMethod::kOptions)};

    LOG_DEBUG() << "Requesting OPTIONS for path " << path;

    for (const auto method : http::kHandlerMethods) {
        auto match_result = GetHandlerInfoIndex().MatchRequest(method, path);
        switch (match_result.status) {
            case http::MatchRequestResult::Status::kOk:
                allowed_methods.push_back(ToString(method));
                break;
            case http::MatchRequestResult::Status::kHandlerNotFound:
                LOG_ERROR() << "No handlers available for path " << path;
                return ToString(http::HttpMethod::kOptions);
            case http::MatchRequestResult::Status::kMethodNotAllowed:
                break;
        }
    }

    std::sort(allowed_methods.begin(), allowed_methods.end());
    return fmt::to_string(fmt::join(allowed_methods, ", "));
}

const http::HandlerInfoIndex& ImplicitOptions::GetHandlerInfoIndex() const {
    if (handler_info_index_) return *handler_info_index_;

    const std::lock_guard lock(handler_info_index_mutex_);

    handler_info_index_ = &server_.GetHttpRequestHandler(IsMonitor()).GetHandlerInfoIndex();
    return *handler_info_index_;
}

std::string ImplicitOptions::HandleRequestThrow(
    const server::http::HttpRequest& request,
    server::request::RequestContext& context
) const {
    auto& response = request.GetHttpResponse();

    response.SetHeader(USERVER_NAMESPACE::http::headers::kAllow, ExtractAllowedMethods(request.GetRequestPath()));

    if (request.HasHeader(USERVER_NAMESPACE::http::headers::kXYaTaxiAllowAuthRequest)) {
        constexpr auto kUnknownChecker = "unknown checker";
        std::optional<std::string> check_status;

        const auto& check_type = request.GetHeader(USERVER_NAMESPACE::http::headers::kXYaTaxiAllowAuthRequest);
        const auto it = auth_checkers_.find(check_type);

        if (it != auth_checkers_.end() && it->second) {
            auto check_result = it->second->CheckAuth(request, context);
            check_status = auth::GetDefaultReasonForStatus(check_result.status);
        } else {
            LOG_WARNING() << "Auth checker for '" << check_type << "' not found, skipping";
        }

        response.SetHeader(
            USERVER_NAMESPACE::http::headers::kXYaTaxiAllowAuthResponse, check_status.value_or(kUnknownChecker)
        );
        response.SetHeader(
            USERVER_NAMESPACE::http::headers::kAccessControlAllowHeaders,
            std::string{USERVER_NAMESPACE::http::headers::kXYaTaxiAllowAuthResponse}
        );
    }

    return {};
}

yaml_config::Schema ImplicitOptions::GetStaticConfigSchema() {
    return yaml_config::MergeSchemas<HttpHandlerBase>(R"(
type: object
description: handler-implicit-http-options config
additionalProperties: false
properties:
    auth_checkers:
        type: object
        description: server::handlers::auth::HandlerAuthConfig authorization config
        defaultDescription: auth checker testing is disabled
        additionalProperties: false
        properties:
            type:
                type: string
                description: auth type
            types:
                type: array
                description: list of auth types
                items:
                    type: string
                    description: auth type
)");
}

}  // namespace server::handlers

USERVER_NAMESPACE_END
