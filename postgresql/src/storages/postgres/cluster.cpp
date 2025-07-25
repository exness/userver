#include <userver/storages/postgres/cluster.hpp>

#include <storages/postgres/detail/cluster_impl.hpp>
#include <storages/postgres/detail/pg_impl_types.hpp>

USERVER_NAMESPACE_BEGIN

namespace storages::postgres {

Cluster::Cluster(
    DsnList dsns,
    clients::dns::Resolver* resolver,
    engine::TaskProcessor& bg_task_processor,
    const ClusterSettings& cluster_settings,
    DefaultCommandControls&& default_cmd_ctls,
    const testsuite::PostgresControl& testsuite_pg_ctl,
    const error_injection::Settings& ei_settings,
    testsuite::TestsuiteTasks& testsuite_tasks,
    dynamic_config::Source config_source,
    USERVER_NAMESPACE::utils::statistics::MetricsStoragePtr metrics,
    int shard_number
) {
    pimpl_ = std::make_unique<detail::ClusterImpl>(
        std::move(dsns),
        resolver,
        bg_task_processor,
        cluster_settings,
        std::move(default_cmd_ctls),
        testsuite_pg_ctl,
        ei_settings,
        testsuite_tasks,
        std::move(config_source),
        std::move(metrics),
        shard_number
    );
}

Cluster::~Cluster() = default;

ClusterStatisticsPtr Cluster::GetStatistics() const { return pimpl_->GetStatistics(); }

Transaction Cluster::Begin(const TransactionOptions& options, OptionalCommandControl cmd_ctl) {
    return Begin({}, options, cmd_ctl);
}

Transaction
Cluster::Begin(ClusterHostTypeFlags flags, const TransactionOptions& options, OptionalCommandControl cmd_ctl) {
    return pimpl_->Begin(flags, options, GetHandlersCmdCtl(cmd_ctl));
}

Transaction Cluster::Begin(std::string name, const TransactionOptions& options) {
    return Begin(std::move(name), {}, options);
}

Transaction Cluster::Begin(std::string name, ClusterHostTypeFlags flags, const TransactionOptions& options) {
    auto trx = pimpl_->Begin(flags, options, GetHandlersCmdCtl(GetQueryCmdCtl(name)));
    trx.SetName(std::move(name));
    return trx;
}

NotifyScope Cluster::Listen(std::string_view channel, OptionalCommandControl cmd_ctl) {
    return pimpl_->Listen(channel, cmd_ctl);
}

QueryQueue Cluster::CreateQueryQueue(ClusterHostTypeFlags flags) {
    return CreateQueryQueue(flags, pimpl_->GetDefaultCommandControl().network_timeout_ms);
}

QueryQueue Cluster::CreateQueryQueue(ClusterHostTypeFlags flags, TimeoutDuration acquire_timeout) {
    return pimpl_->CreateQueryQueue(flags, acquire_timeout);
}

void Cluster::SetDefaultCommandControl(CommandControl cmd_ctl) {
    pimpl_->SetDefaultCommandControl(cmd_ctl, detail::DefaultCommandControlSource::kUser);
}

CommandControl Cluster::GetDefaultCommandControl() const { return pimpl_->GetDefaultCommandControl(); }

void Cluster::SetHandlersCommandControl(CommandControlByHandlerMap handlers_command_control) {
    pimpl_->SetHandlersCommandControl(std::move(handlers_command_control));
}

void Cluster::SetQueriesCommandControl(CommandControlByQueryMap queries_command_control) {
    pimpl_->SetQueriesCommandControl(std::move(queries_command_control));
}

void Cluster::ApplyGlobalCommandControlUpdate(CommandControl cmd_ctl) {
    pimpl_->SetDefaultCommandControl(cmd_ctl, detail::DefaultCommandControlSource::kGlobalConfig);
}

void Cluster::SetConnectionSettings(const ConnectionSettings& settings) { pimpl_->SetConnectionSettings(settings); }

void Cluster::SetPoolSettings(const PoolSettings& settings) { pimpl_->SetPoolSettings(settings); }

void Cluster::SetTopologySettings(const TopologySettings& settings) { pimpl_->SetTopologySettings(settings); }

void Cluster::SetStatementMetricsSettings(const StatementMetricsSettings& settings) {
    pimpl_->SetStatementMetricsSettings(settings);
}

void Cluster::SetDsnList(const DsnList& dsn) { pimpl_->SetDsnList(dsn); }

detail::NonTransaction Cluster::Start(ClusterHostTypeFlags flags, OptionalCommandControl cmd_ctl) {
    return pimpl_->Start(flags, cmd_ctl);
}

OptionalCommandControl Cluster::GetQueryCmdCtl(std::string_view query_name) const {
    return pimpl_->GetQueryCmdCtl(query_name);
}

OptionalCommandControl Cluster::GetHandlersCmdCtl(OptionalCommandControl cmd_ctl) const {
    return cmd_ctl ? cmd_ctl : pimpl_->GetTaskDataHandlersCommandControl();
}

ResultSet Cluster::Execute(ClusterHostTypeFlags flags, const Query& query, const ParameterStore& store) {
    return Execute(flags, OptionalCommandControl{}, query, store);
}

ResultSet Cluster::Execute(
    ClusterHostTypeFlags flags,
    OptionalCommandControl statement_cmd_ctl,
    const Query& query,
    const ParameterStore& store
) {
    if (!statement_cmd_ctl && query.GetOptionalNameView()) {
        statement_cmd_ctl = GetQueryCmdCtl(*query.GetOptionalNameView());
    }
    statement_cmd_ctl = GetHandlersCmdCtl(statement_cmd_ctl);
    auto ntrx = Start(flags, statement_cmd_ctl);
    return ntrx.Execute(statement_cmd_ctl, query.GetStatementView(), store);
}

}  // namespace storages::postgres

USERVER_NAMESPACE_END
