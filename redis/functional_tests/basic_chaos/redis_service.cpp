#include <userver/utest/using_namespace_userver.hpp>  // IWYU pragma: keep

#include <string>
#include <string_view>

#include <fmt/format.h>

#include <userver/clients/dns/component.hpp>
#include <userver/clients/http/component.hpp>
#include <userver/components/component.hpp>
#include <userver/components/minimal_server_component_list.hpp>
#include <userver/dynamic_config/client/component.hpp>
#include <userver/dynamic_config/updater/component.hpp>
#include <userver/engine/sleep.hpp>
#include <userver/server/handlers/http_handler_base.hpp>
#include <userver/server/handlers/server_monitor.hpp>
#include <userver/server/handlers/tests_control.hpp>
#include <userver/storages/redis/client.hpp>
#include <userver/storages/redis/component.hpp>
#include <userver/storages/secdist/component.hpp>
#include <userver/storages/secdist/provider_component.hpp>
#include <userver/testsuite/testsuite_support.hpp>
#include <userver/utils/daemon_run.hpp>
#include <userver/utils/from_string.hpp>

namespace chaos {

class KeyValue final : public server::handlers::HttpHandlerBase {
public:
    static constexpr std::string_view kName = "handler-chaos";

    KeyValue(const components::ComponentConfig& config, const components::ComponentContext& context);

    std::string HandleRequestThrow(const server::http::HttpRequest& request, server::request::RequestContext&)
        const override;

private:
    std::string GetValue(std::string_view key, const server::http::HttpRequest& request) const;
    std::string PostValue(std::string_view key, const server::http::HttpRequest& request) const;
    std::string DeleteValue(std::string_view key) const;

    storages::redis::ClientPtr redis_client_;
    storages::redis::CommandControl redis_cc_;
};

KeyValue::KeyValue(const components::ComponentConfig& config, const components::ComponentContext& context)
    : server::handlers::HttpHandlerBase(config, context),
      redis_client_{context.FindComponent<components::Redis>("key-value-database").GetClient("test")},
      redis_cc_{std::chrono::seconds{15}, std::chrono::seconds{60}, 4} {}

std::string
KeyValue::HandleRequestThrow(const server::http::HttpRequest& request, server::request::RequestContext& /*context*/)
    const {
    const auto& key = request.GetArg("key");
    if (key.empty()) {
        throw server::handlers::ClientError(server::handlers::ExternalBody{"No 'key' query argument"});
    }

    const auto& sleep_ms = request.GetArg("sleep_ms");
    if (!sleep_ms.empty()) {
        LOG_DEBUG() << "Sleep for " << sleep_ms << "ms";
        const std::chrono::milliseconds ms{utils::FromString<int>(sleep_ms)};
        engine::SleepFor(ms);
    }

    switch (request.GetMethod()) {
        case server::http::HttpMethod::kGet:
            return GetValue(key, request);
        case server::http::HttpMethod::kPost:
            return PostValue(key, request);
        case server::http::HttpMethod::kDelete:
            return DeleteValue(key);
        default:
            throw server::handlers::ClientError(server::handlers::ExternalBody{
                fmt::format("Unsupported method {}", request.GetMethod())});
    }
}

std::string KeyValue::GetValue(std::string_view key, const server::http::HttpRequest& request) const {
    auto redis_request = redis_client_->Get(std::string{key}, redis_cc_);
    try {
        const auto result = redis_request.Get();
        if (!result) {
            request.SetResponseStatus(server::http::HttpStatus::kNotFound);
            return {};
        }
        return *result;
    } catch (const storages::redis::RequestFailedException& e) {
        if (e.GetStatus() == storages::redis::ReplyStatus::kTimeoutError) {
            request.SetResponseStatus(server::http::HttpStatus::kServiceUnavailable);
            return "timeout";
        }

        throw;
    }
}

std::string KeyValue::PostValue(std::string_view key, const server::http::HttpRequest& request) const {
    const auto& value = request.GetArg("value");
    auto redis_request = redis_client_->SetIfNotExist(std::string{key}, value, redis_cc_);

    const auto result = redis_request.Get();
    if (!result) {
        request.SetResponseStatus(server::http::HttpStatus::kConflict);
        return {};
    }

    request.SetResponseStatus(server::http::HttpStatus::kCreated);
    return std::string{value};
}

std::string KeyValue::DeleteValue(std::string_view key) const {
    const auto result = redis_client_->Del(std::string{key}, redis_cc_).Get();
    return std::to_string(result);
}

class MakeManyRequests final : public server::handlers::HttpHandlerBase {
public:
    static constexpr std::string_view kName = "handler-chaos-many-requests";

    MakeManyRequests(const components::ComponentConfig& config, const components::ComponentContext& context);

    std::string HandleRequestThrow(const server::http::HttpRequest& request, server::request::RequestContext&)
        const override;

private:
    std::string GetValue(std::string_view key, const server::http::HttpRequest& request) const;

    storages::redis::ClientPtr redis_client_;
    storages::redis::CommandControl redis_cc_;
};

MakeManyRequests::MakeManyRequests(
    const components::ComponentConfig& config,
    const components::ComponentContext& context
)
    : server::handlers::HttpHandlerBase(config, context),
      redis_client_{context.FindComponent<components::Redis>("key-value-database").GetClient("test")},
      redis_cc_{std::chrono::seconds{15}, std::chrono::seconds{60}, 4} {
    redis_cc_.allow_reads_from_master = true;
}

std::string MakeManyRequests::
    HandleRequestThrow(const server::http::HttpRequest& request, server::request::RequestContext& /*context*/) const {
    constexpr size_t kRequestsCount = 1000;

    auto cc = redis_cc_;

    const auto& consider_ping = request.GetArg("consider_ping");
    if (consider_ping == "False") {
        LOG_DEBUG() << "Consider ping: False";
        cc.consider_ping = false;
    }

    std::vector<storages::redis::RequestGet> requests;
    requests.reserve(kRequestsCount);
    for (size_t i = 0; i < kRequestsCount; ++i) {
        requests.push_back(redis_client_->Get(std::string("some_key"), cc));
    }

    for (auto& redis_request : requests) {
        try {
            const auto result = redis_request.Get();
        } catch (const storages::redis::RequestFailedException& e) {
            if (e.GetStatus() == storages::redis::ReplyStatus::kTimeoutError) {
                request.SetResponseStatus(server::http::HttpStatus::kServiceUnavailable);
                return "timeout";
            }

            throw;
        }
    }
    return "ok";
}

}  // namespace chaos

int main(int argc, char* argv[]) {
    const auto component_list = components::MinimalServerComponentList()
                                    .Append<chaos::KeyValue>()
                                    .Append<chaos::MakeManyRequests>()
                                    .Append<server::handlers::ServerMonitor>()
                                    .Append<components::Secdist>()
                                    .Append<components::DefaultSecdistProvider>()
                                    .Append<components::Redis>("key-value-database")
                                    .Append<components::TestsuiteSupport>()
                                    .Append<clients::dns::Component>()
                                    .Append<components::HttpClient>()
                                    .Append<server::handlers::TestsControl>()
                                    .Append<components::DynamicConfigClient>()
                                    .Append<components::DynamicConfigClientUpdater>();
    return utils::DaemonMain(argc, argv, component_list);
}
