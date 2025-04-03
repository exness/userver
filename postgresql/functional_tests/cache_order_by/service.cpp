#include <userver/clients/dns/component.hpp>
#include <userver/testsuite/testsuite_support.hpp>

#include <userver/utest/using_namespace_userver.hpp>

#include <userver/clients/http/component.hpp>
#include <userver/components/minimal_server_component_list.hpp>
#include <userver/server/handlers/http_handler_base.hpp>
#include <userver/server/handlers/server_monitor.hpp>
#include <userver/server/handlers/tests_control.hpp>
#include <userver/testsuite/testpoint.hpp>
#include <userver/utils/daemon_run.hpp>

#include <userver/storages/postgres/cluster.hpp>
#include <userver/storages/postgres/component.hpp>
#include <userver/storages/postgres/dist_lock_component_base.hpp>

#include <userver/cache/base_postgres_cache.hpp>

namespace pg::cache_order_by {

struct KeyValue {
    std::string key;
    std::string value;
};

struct LastCachePolicy {
    static constexpr std::string_view kName = "last-pg-cache";

    using ValueType = KeyValue;
    static constexpr auto kKeyMember = &KeyValue::key;
    static constexpr const char* kQuery = "SELECT DISTINCT ON (key) key, value FROM key_value_table";
    static constexpr const char* kUpdatedField = "updated";
    using UpdatedFieldType = storages::postgres::TimePointTz;
    static constexpr const char* kOrderBy = "key, updated DESC";
};

using LastCache = components::PostgreCache<LastCachePolicy>;

struct FirstCachePolicy {
    static constexpr std::string_view kName = "first-pg-cache";

    using ValueType = KeyValue;
    static constexpr auto kKeyMember = &KeyValue::key;
    static constexpr const char* kQuery = "SELECT DISTINCT ON (key) key, value FROM key_value_table";
    static constexpr const char* kUpdatedField = "updated";
    using UpdatedFieldType = storages::postgres::TimePointTz;
    static constexpr const char* kOrderBy = "key, updated ASC";
};

using FirstCache = components::PostgreCache<FirstCachePolicy>;

class CacheHandler final : public server::handlers::HttpHandlerJsonBase {
public:
    static constexpr std::string_view kName = "handler-cache-order-by-postgres";

    CacheHandler(const components::ComponentConfig& config, const components::ComponentContext& context);

    formats::json::Value
    HandleRequestJsonThrow(const server::http::HttpRequest& request, const formats::json::Value&, server::request::RequestContext&)
        const override;

private:
    const FirstCache& first_cache_;
    const LastCache& last_cache_;
};

CacheHandler::CacheHandler(const components::ComponentConfig& config, const components::ComponentContext& context)
    : HttpHandlerJsonBase(config, context),
      first_cache_{context.FindComponent<FirstCache>()},
      last_cache_{context.FindComponent<LastCache>()} {}

formats::json::Value CacheHandler::
    HandleRequestJsonThrow(const server::http::HttpRequest& request, const formats::json::Value&, server::request::RequestContext&)
        const {
    const auto& key = request.GetArg("key");
    const auto& order = request.GetArg("order");
    if (order == "first") {
        const auto cache_data = first_cache_.Get();
        return formats::json::MakeObject("result", cache_data->at(key).value);
    }
    if (order == "last") {
        const auto cache_data = last_cache_.Get();
        return formats::json::MakeObject("result", cache_data->at(key).value);
    }
    return {};
}

}  // namespace pg::cache_order_by

int main(int argc, char* argv[]) {
    const auto component_list = components::MinimalServerComponentList()
                                    .Append<server::handlers::ServerMonitor>()
                                    .Append<pg::cache_order_by::CacheHandler>()
                                    .Append<pg::cache_order_by::FirstCache>()
                                    .Append<pg::cache_order_by::LastCache>()
                                    .Append<components::HttpClient>()
                                    .Append<components::Postgres>("key-value-database")
                                    .Append<components::TestsuiteSupport>()
                                    .Append<server::handlers::TestsControl>()
                                    .Append<clients::dns::Component>();
    return utils::DaemonMain(argc, argv, component_list);
}
