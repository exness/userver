#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include <userver/clients/dns/resolver_fwd.hpp>
#include <userver/dynamic_config/source.hpp>
#include <userver/engine/task/task_processor_fwd.hpp>
#include <userver/error_injection/settings.hpp>
#include <userver/testsuite/postgres_control.hpp>
#include <userver/testsuite/tasks.hpp>

#include <storages/postgres/connlimit_watchdog.hpp>
#include <storages/postgres/detail/pg_impl_types.hpp>
#include <storages/postgres/detail/pool.hpp>
#include <storages/postgres/detail/statement_stats_storage.hpp>
#include <storages/postgres/detail/topology/base.hpp>
#include <userver/storages/postgres/cluster_types.hpp>
#include <userver/storages/postgres/detail/non_transaction.hpp>
#include <userver/storages/postgres/notify.hpp>
#include <userver/storages/postgres/options.hpp>
#include <userver/storages/postgres/query_queue.hpp>
#include <userver/storages/postgres/statistics.hpp>
#include <userver/storages/postgres/transaction.hpp>

USERVER_NAMESPACE_BEGIN

namespace storages::postgres::detail {

class ClusterImpl {
public:
    ClusterImpl(
        DsnList dsns,
        clients::dns::Resolver* resolver,
        engine::TaskProcessor& bg_task_processor,
        const ClusterSettings& cluster_settings,
        const DefaultCommandControls& default_cmd_ctls,
        const testsuite::PostgresControl& testsuite_pg_ctl,
        const error_injection::Settings& ei_settings,
        testsuite::TestsuiteTasks& testsuite_tasks,
        dynamic_config::Source config_source,
        USERVER_NAMESPACE::utils::statistics::MetricsStoragePtr metrics,
        int shard_number
    );

    ~ClusterImpl();

    ClusterStatisticsPtr GetStatistics() const;

    Transaction Begin(ClusterHostTypeFlags, const TransactionOptions&, OptionalCommandControl);

    NonTransaction Start(ClusterHostTypeFlags, OptionalCommandControl);

    NotifyScope Listen(std::string_view channel, OptionalCommandControl);

    QueryQueue CreateQueryQueue(ClusterHostTypeFlags flags, TimeoutDuration acquire_timeout);

    void SetDefaultCommandControl(CommandControl, DefaultCommandControlSource);
    CommandControl GetDefaultCommandControl() const;

    void SetHandlersCommandControl(CommandControlByHandlerMap&& handlers_command_control);

    void SetQueriesCommandControl(CommandControlByQueryMap&& queries_command_control);

    void SetConnectionSettings(const ConnectionSettings& settings);

    void SetPoolSettings(const PoolSettings& settings);

    void SetTopologySettings(const TopologySettings& settings);

    void SetStatementMetricsSettings(const StatementMetricsSettings& settings);

    OptionalCommandControl GetQueryCmdCtl(std::string_view query_name) const;

    OptionalCommandControl GetTaskDataHandlersCommandControl() const;

    std::string GetDbName() const;

    void SetDsnList(const DsnList&);

private:
    void OnConnlimitChanged();

    bool IsConnlimitModeAuto(const ClusterSettings& settings);

    using ConnectionPoolPtr = std::shared_ptr<ConnectionPool>;

    ConnectionPoolPtr FindPool(ClusterHostTypeFlags);

    struct TopologyData {
        std::unique_ptr<topology::TopologyBase> topology;
        std::vector<ConnectionPoolPtr> host_pools;
    };

    void CreateTopology(const DsnList& dsns);

    rcu::Variable<ClusterSettings> cluster_settings_;
    concurrent::Variable<TopologyData, engine::SharedMutex> topology_data_;
    clients::dns::Resolver* resolver_{};
    engine::TaskProcessor& bg_task_processor_;
    dynamic_config::Source config_source_;
    DefaultCommandControls default_cmd_ctls_;
    const testsuite::PostgresControl testsuite_pg_ctl_;
    const error_injection::Settings ei_settings_;
    USERVER_NAMESPACE::utils::statistics::MetricsStoragePtr metrics_;

    std::atomic<uint32_t> rr_host_idx_;
    std::atomic<bool> connlimit_mode_auto_enabled_;
    ConnlimitWatchdog connlimit_watchdog_;
};

}  // namespace storages::postgres::detail

USERVER_NAMESPACE_END
