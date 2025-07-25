#include <userver/storages/mongo/component.hpp>

#include <boost/algorithm/string/predicate.hpp>

#include <userver/clients/dns/resolver_utils.hpp>
#include <userver/components/component.hpp>
#include <userver/components/statistics_storage.hpp>
#include <userver/dynamic_config/storage/component.hpp>
#include <userver/storages/mongo/exception.hpp>
#include <userver/storages/mongo/pool_config.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

#include <storages/mongo/mongo_secdist.hpp>

USERVER_NAMESPACE_BEGIN

namespace components {

namespace {
const std::string kStandardMongoPrefix = "mongo-";

auto ParsePoolConfig(const ComponentConfig& config) {
    auto pool_config = config.As<storages::mongo::PoolConfig>();
    pool_config.Validate(config.Name());
    return pool_config;
}

}  // namespace

Mongo::Mongo(const ComponentConfig& config, const ComponentContext& context) : ComponentBase(config, context) {
    auto dbalias = config["dbalias"].As<std::string>("");

    std::string connection_string;
    storages::secdist::Secdist* secdist{};
    if (!dbalias.empty()) {
        dbalias_ = dbalias;
        secdist = &context.FindComponent<Secdist>().GetStorage();
        connection_string = storages::mongo::secdist::GetSecdistConnectionString(*secdist, dbalias);
    } else {
        connection_string = config["dbconnection"].As<std::string>();
    }

    auto* dns_resolver = clients::dns::GetResolverPtr(config, context);

    const auto pool_config = ParsePoolConfig(config);
    auto config_source = context.FindComponent<DynamicConfig>().GetSource();

    pool_ = std::make_shared<storages::mongo::Pool>(
        config.Name(), connection_string, pool_config, dns_resolver, config_source
    );

    if (!dbalias_.empty()) {
        secdist_subscriber_ = secdist->UpdateAndListen(this, dbalias_, &Mongo::OnSecdistUpdate);
    }

    auto& statistics_storage = context.FindComponent<components::StatisticsStorage>();

    auto section_name = config.Name();
    if (boost::algorithm::starts_with(section_name, kStandardMongoPrefix) &&
        section_name.size() != kStandardMongoPrefix.size()) {
        section_name = section_name.substr(kStandardMongoPrefix.size());
    }
    statistics_holder_ = statistics_storage.GetStorage().RegisterWriter(
        "mongo",
        [this](utils::statistics::Writer& writer) {
            UASSERT(pool_);
            writer = *pool_;
        },
        {{"mongo_database", std::move(section_name)}}
    );
}

Mongo::~Mongo() {
    statistics_holder_.Unregister();
    secdist_subscriber_.Unsubscribe();
}

storages::mongo::PoolPtr Mongo::GetPool() const { return pool_; }

void Mongo::OnSecdistUpdate(const storages::secdist::SecdistConfig& config) {
    auto connection_string = storages::mongo::secdist::GetSecdistConnectionString(config, dbalias_);
    pool_->SetConnectionString(connection_string);
}

yaml_config::Schema Mongo::GetStaticConfigSchema() {
    return yaml_config::MergeSchemas<MultiMongo>(R"(
type: object
description: MongoDB client component
additionalProperties: false
properties:
    dbalias:
        type: string
        description: name of the database in secdist config (if available)
    dbconnection:
        type: string
        description: connection string (used if no dbalias specified)
    maintenance_period:
        type: string
        description: pool maintenance period (idle connections pruning etc.)
        defaultDescription: 15s
)");
}

MultiMongo::MultiMongo(const ComponentConfig& config, const ComponentContext& context)
    : ComponentBase(config, context),
      multi_mongo_(
          config.Name(),
          context.FindComponent<Secdist>().GetStorage(),
          ParsePoolConfig(config),
          clients::dns::GetResolverPtr(config, context),
          context.FindComponent<DynamicConfig>().GetSource()
      ) {
    auto& statistics_storage = context.FindComponent<components::StatisticsStorage>();
    statistics_holder_ = statistics_storage.GetStorage().RegisterWriter(
        multi_mongo_.GetName(), [this](utils::statistics::Writer& writer) { writer = multi_mongo_; }
    );
}

MultiMongo::~MultiMongo() { statistics_holder_.Unregister(); }

storages::mongo::PoolPtr MultiMongo::GetPool(const std::string& dbalias) const { return multi_mongo_.GetPool(dbalias); }

void MultiMongo::AddPool(std::string dbalias) { multi_mongo_.AddPool(std::move(dbalias)); }

bool MultiMongo::RemovePool(const std::string& dbalias) { return multi_mongo_.RemovePool(dbalias); }

storages::mongo::MultiMongo::PoolSet MultiMongo::NewPoolSet() { return multi_mongo_.NewPoolSet(); }

yaml_config::Schema MultiMongo::GetStaticConfigSchema() {
    return yaml_config::MergeSchemas<ComponentBase>(R"(
type: object
description: Dynamically configurable MongoDB client component
additionalProperties: false
properties:
    appname:
        type: string
        description: application name for the DB server
        defaultDescription: userver
    conn_timeout:
        type: string
        description: connection timeout
        defaultDescription: 2s
    so_timeout:
        type: string
        description: socket timeout
        defaultDescription: 10s
    queue_timeout:
        type: string
        description: max connection queue wait time
        defaultDescription: 1s
    initial_size:
        type: string
        description: number of connections created initially (per database)
        defaultDescription: 16
    max_size:
        type: integer
        description: limit for total connections number (per database)
        defaultDescription: 128
    idle_limit:
        type: integer
        description: limit for idle connections number (per database)
        defaultDescription: 64
    connecting_limit:
        type: integer
        description: limit for establishing connections number (per database)
        defaultDescription: 8
    local_threshold:
        type: string
        description: latency window for instance selection
        defaultDescription: mongodb default
    max_replication_lag:
        type: string
        description: replication lag limit for usable secondaries, min. 90s
    stats_verbosity:
        type: string
        description: changes the granularity of reported metrics
        defaultDescription: 'terse'
        enum:
          - terse
          - full
          - none
    dns_resolver:
        type: string
        description: server hostname resolver type (getaddrinfo or async)
        defaultDescription: 'async'
        enum:
          - getaddrinfo
          - async
    congestion_control:
        description: congestion control settings
        type: object
        additionalProperties: false
        properties:
            fake-mode:
                type: boolean
                description: whether CC limiter is actually working
                defaultDescription: false
            enabled:
                type: boolean
                description: whether CC is enabled for the database
                defaultDescription: true
)");
}

}  // namespace components

USERVER_NAMESPACE_END
