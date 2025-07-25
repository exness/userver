#pragma once
#include "sentinel_impl.hpp"

USERVER_NAMESPACE_BEGIN

namespace engine::ev {
class PeriodicWatcher;
}

namespace storages::redis::impl {
class TopologyHolderBase;

class ClusterSentinelImpl : public SentinelImplBase {
public:
    using ReadyChangeCallback = std::function<void(size_t shard, const std::string& shard_name, bool ready)>;
    using SentinelCommand = SentinelImplBase::SentinelCommand;

    static constexpr std::size_t kUnknownShard = std::numeric_limits<std::size_t>::max();
    ClusterSentinelImpl(
        const engine::ev::ThreadControl& sentinel_thread_control,
        const std::shared_ptr<engine::ev::ThreadPool>& redis_thread_pool,
        Sentinel& sentinel,
        const std::vector<std::string>& shards,
        const std::vector<ConnectionInfo>& conns,
        std::string shard_group_name,
        const std::string& client_name,
        const Password& password,
        ConnectionSecurity connection_security,
        std::unique_ptr<KeyShard>&& key_shard,
        dynamic_config::Source dynamic_config_source
    );
    ~ClusterSentinelImpl() override;

    std::unordered_map<ServerId, size_t, ServerIdHasher>
    GetAvailableServersWeighted(size_t shard_idx, bool with_master, const CommandControl& cc /*= {}*/) const override;

    void WaitConnectedDebug(bool allow_empty_slaves) override;

    void WaitConnectedOnce(RedisWaitConnected wait_connected) override;

    void ForceUpdateHosts() override;

    void AsyncCommand(const SentinelCommand& scommand, size_t prev_instance_idx /*= -1*/) override;

    size_t ShardByKey(const std::string& key) const override;

    size_t ShardsCount() const override;

    SentinelStatistics GetStatistics(const MetricsSettings& settings) const override;

    void Start() override;
    void Stop() override;

    void SetCommandsBufferingSettings(CommandsBufferingSettings commands_buffering_settings) override;
    void SetReplicationMonitoringSettings(const ReplicationMonitoringSettings& replication_monitoring_settings
    ) override;
    void SetRetryBudgetSettings(const utils::RetryBudgetSettings& settings) override;
    PublishSettings GetPublishSettings() override;

    static size_t GetClusterSlotsCalledCounter();

    void SetConnectionInfo(const std::vector<ConnectionInfoInt>& info_array) override;

    void UpdatePassword(const Password& password) override;

private:
    void Init();  // used from constructor

    void AsyncCommandFailed(const SentinelCommand& scommand);
    void EnqueueCommand(const SentinelCommand& command);

    Sentinel& sentinel_obj_;
    engine::ev::ThreadControl ev_thread_;

    std::unique_ptr<engine::ev::PeriodicWatcher> process_waiting_commands_timer_;
    void ProcessWaitingCommands();
    void ProcessWaitingCommandsOnStop();

    std::unique_ptr<TopologyHolderBase> topology_holder_;

    const std::string shard_group_name_;
    const std::vector<ConnectionInfo> conns_;

    std::shared_ptr<engine::ev::ThreadPool> redis_thread_pool_;

    const std::string client_name_;

    std::vector<SentinelCommand> commands_;
    std::mutex command_mutex_;

    SentinelStatisticsInternal statistics_internal_;

    dynamic_config::Source dynamic_config_source_;
};

}  // namespace storages::redis::impl

USERVER_NAMESPACE_END
