#include <userver/clients/dns/component.hpp>
#include <userver/testsuite/testsuite_support.hpp>

#include <userver/utest/using_namespace_userver.hpp>

/// [Postgres service sample - component]
#include <userver/clients/http/component.hpp>
#include <userver/components/component.hpp>
#include <userver/components/minimal_server_component_list.hpp>
#include <userver/server/handlers/http_handler_base.hpp>
#include <userver/server/handlers/tests_control.hpp>
#include <userver/utils/daemon_run.hpp>

#include <userver/storages/postgres/cluster.hpp>
#include <userver/storages/postgres/component.hpp>

#include <samples_postgres_service/sql_queries.hpp>

namespace samples_postgres_service::pg {

class KeyValue final : public server::handlers::HttpHandlerBase {
public:
    static constexpr std::string_view kName = "handler-key-value";

    KeyValue(const components::ComponentConfig& config, const components::ComponentContext& context);

    std::string HandleRequest(server::http::HttpRequest& request, server::request::RequestContext&) const override;

private:
    std::string GetValue(std::string_view key, const server::http::HttpRequest& request) const;
    std::string PostValue(std::string_view key, const server::http::HttpRequest& request) const;
    std::string DeleteValue(std::string_view key) const;

    storages::postgres::ClusterPtr pg_cluster_;
};

}  // namespace samples_postgres_service::pg
/// [Postgres service sample - component]

namespace samples_postgres_service::pg {

/// [Postgres service sample - component constructor]
KeyValue::KeyValue(const components::ComponentConfig& config, const components::ComponentContext& context)
    : HttpHandlerBase(config, context),
      pg_cluster_(context.FindComponent<components::Postgres>("key-value-database").GetCluster()) {}
/// [Postgres service sample - component constructor]

/// [Postgres service sample - HandleRequestThrow]
std::string KeyValue::HandleRequest(server::http::HttpRequest& request, server::request::RequestContext&) const {
    const auto& key = request.GetArg("key");
    if (key.empty()) {
        throw server::handlers::ClientError(server::handlers::ExternalBody{"No 'key' query argument"});
    }

    request.GetHttpResponse().SetContentType(http::content_type::kTextPlain);
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
/// [Postgres service sample - HandleRequestThrow]

/// [Postgres service sample - GetValue]
std::string KeyValue::GetValue(std::string_view key, const server::http::HttpRequest& request) const {
    const storages::postgres::ResultSet res =
        pg_cluster_->Execute(storages::postgres::ClusterHostType::kSlave, sql::kSelectValue, key);
    if (res.IsEmpty()) {
        request.SetResponseStatus(server::http::HttpStatus::kNotFound);
        return {};
    }

    return res.AsSingleRow<std::string>();
}
/// [Postgres service sample - GetValue]

/// [Postgres service sample - PostValue]
std::string KeyValue::PostValue(std::string_view key, const server::http::HttpRequest& request) const {
    const auto& value = request.GetArg("value");

    storages::postgres::Transaction transaction =
        pg_cluster_->Begin("sample_transaction_insert_key_value", storages::postgres::ClusterHostType::kMaster, {});

    auto res = transaction.Execute(sql::kInsertValue, key, value);
    if (res.RowsAffected()) {
        transaction.Commit();
        request.SetResponseStatus(server::http::HttpStatus::kCreated);
        return std::string{value};
    }

    res = transaction.Execute(sql::kSelectValue, key);
    transaction.Rollback();

    auto result = res.AsSingleRow<std::string>();
    if (result != value) {
        request.SetResponseStatus(server::http::HttpStatus::kConflict);
    }

    return res.AsSingleRow<std::string>();
}
/// [Postgres service sample - PostValue]

/// [Postgres service sample - DeleteValue]
std::string KeyValue::DeleteValue(std::string_view key) const {
    auto res = pg_cluster_->Execute(storages::postgres::ClusterHostType::kMaster, sql::kDeleteValue, key);
    return std::to_string(res.RowsAffected());
}
/// [Postgres service sample - DeleteValue]

}  // namespace samples_postgres_service::pg

/// [Postgres service sample - main]
int main(int argc, char* argv[]) {
    const auto component_list = components::MinimalServerComponentList()
                                    .Append<samples_postgres_service::pg::KeyValue>()
                                    .Append<components::Postgres>("key-value-database")
                                    .Append<components::HttpClient>()
                                    .Append<components::TestsuiteSupport>()
                                    .Append<server::handlers::TestsControl>()
                                    .Append<clients::dns::Component>();
    return utils::DaemonMain(argc, argv, component_list);
}
/// [Postgres service sample - main]
