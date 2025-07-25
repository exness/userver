#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/signals2/signal.hpp>

#include <engine/ev/thread_control.hpp>
#include <engine/ev/thread_pool.hpp>
#include <userver/concurrent/variable.hpp>
#include <userver/dynamic_config/source.hpp>
#include <userver/engine/deadline.hpp>
#include <userver/engine/impl/condition_variable_any.hpp>
#include <userver/utils/retry_budget.hpp>
#include <userver/utils/swappingsmart.hpp>

#include <storages/redis/impl/redis_stats.hpp>
#include <userver/storages/redis/client.hpp>
#include <userver/storages/redis/fwd.hpp>
#include <userver/storages/redis/wait_connected_mode.hpp>

#include "ev_wrapper.hpp"
#include "keys_for_shards.hpp"
#include "keyshard_impl.hpp"
#include "redis.hpp"
#include "sentinel_query.hpp"
#include "shard.hpp"

USERVER_NAMESPACE_BEGIN

namespace storages::redis::impl {

class SentinelImplBase {
public:
    static constexpr size_t kDefaultPrevInstanceIdx = std::numeric_limits<std::size_t>::max();
    static constexpr size_t kUnknownShard = std::numeric_limits<std::size_t>::max();

    struct SentinelCommand {
        CommandPtr command;
        bool master = true;
        size_t shard = kUnknownShard;
        std::chrono::steady_clock::time_point start;

        SentinelCommand() = default;
        SentinelCommand(CommandPtr command, bool master, size_t shard, std::chrono::steady_clock::time_point start)
            : command(command), master(master), shard(shard), start(start) {}
    };

    SentinelImplBase() = default;
    virtual ~SentinelImplBase() = default;

    virtual std::unordered_map<ServerId, size_t, ServerIdHasher>
    GetAvailableServersWeighted(size_t shard_idx, bool with_master, const CommandControl& cc) const = 0;

    virtual void WaitConnectedDebug(bool allow_empty_slaves) = 0;

    virtual void WaitConnectedOnce(RedisWaitConnected wait_connected) = 0;

    virtual void ForceUpdateHosts() = 0;

    virtual void AsyncCommand(const SentinelCommand& scommand, size_t prev_instance_idx) = 0;
    virtual size_t ShardByKey(const std::string& key) const = 0;
    virtual size_t ShardsCount() const = 0;
    virtual SentinelStatistics GetStatistics(const MetricsSettings& settings) const = 0;

    virtual void Start() = 0;
    virtual void Stop() = 0;

    virtual void SetCommandsBufferingSettings(CommandsBufferingSettings commands_buffering_settings) = 0;
    virtual void SetReplicationMonitoringSettings(const ReplicationMonitoringSettings& replication_monitoring_settings
    ) = 0;
    virtual void SetRetryBudgetSettings(const utils::RetryBudgetSettings& retry_budget_settings) = 0;

    virtual PublishSettings GetPublishSettings() = 0;
    virtual void SetConnectionInfo(const std::vector<ConnectionInfoInt>& info_array) = 0;

    virtual void UpdatePassword(const Password& password) = 0;
};

bool AdjustDeadline(const SentinelImplBase::SentinelCommand& scommand, const dynamic_config::Snapshot& config);

class SentinelImpl : public SentinelImplBase {
public:
    using ReadyChangeCallback = std::function<void(size_t shard, const std::string& shard_name, bool ready)>;

    SentinelImpl(
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
        dynamic_config::Source dynamic_config_source,
        std::size_t database_index
    );
    ~SentinelImpl() override;

    std::unordered_map<ServerId, size_t, ServerIdHasher>
    GetAvailableServersWeighted(size_t shard_idx, bool with_master, const CommandControl& cc) const override;

    void WaitConnectedDebug(bool allow_empty_slaves) override;

    void WaitConnectedOnce(RedisWaitConnected wait_connected) override;

    void ForceUpdateHosts() override;

    void AsyncCommand(const SentinelCommand& scommand, size_t prev_instance_idx) override;
    size_t ShardByKey(const std::string& key) const override;
    size_t ShardsCount() const override { return master_shards_.size(); }
    SentinelStatistics GetStatistics(const MetricsSettings& settings) const override;

    void Start() override;
    void Stop() override;

    void SetCommandsBufferingSettings(CommandsBufferingSettings commands_buffering_settings) override;
    void SetReplicationMonitoringSettings(const ReplicationMonitoringSettings& replication_monitoring_settings
    ) override;
    void SetRetryBudgetSettings(const utils::RetryBudgetSettings& retry_budget_settings) override;
    PublishSettings GetPublishSettings() override;

    void SetConnectionInfo(const std::vector<ConnectionInfoInt>& info_array) override;

    void UpdatePassword(const Password& password) override;

private:
    void Init();  // used from constructor

    class ShardInfo {
    public:
        using HostPortToShardMap = std::map<std::pair<std::string, size_t>, size_t>;

        size_t GetShard(const std::string& host, int port) const;
        void UpdateHostPortToShard(HostPortToShardMap&& host_port_to_shard_new);

    private:
        HostPortToShardMap host_port_to_shard_;
        mutable std::mutex mutex_;
    };

    class ConnectedStatus {
    public:
        void SetMasterReady();
        void SetSlaveReady();

        bool WaitReady(engine::Deadline deadline, WaitConnectedMode mode);

    private:
        template <typename Pred>
        bool Wait(engine::Deadline deadline, const Pred& pred);

        std::mutex mutex_;
        std::atomic<bool> master_ready_{false};
        std::atomic<bool> slave_ready_{false};

        engine::impl::ConditionVariableAny<std::mutex> cv_;
    };

    void AsyncCommandFailed(const SentinelCommand& scommand);

    static void OnCheckTimer(struct ev_loop*, ev_timer* w, int revents) noexcept;
    static void ChangedState(struct ev_loop*, ev_async* w, int revents) noexcept;
    static void UpdateInstances(struct ev_loop*, ev_async* w, int revents) noexcept;
    static void OnModifyConnectionInfo(struct ev_loop*, ev_async* w, int revents) noexcept;

    void ProcessCreationOfShards(std::vector<std::shared_ptr<Shard>>& shards);

    void RefreshConnectionInfo();
    void ReadSentinels();
    void CheckConnections();
    void UpdateInstancesImpl();
    bool SetConnectionInfo(ConnInfoMap info_by_shards, std::vector<std::shared_ptr<Shard>>& shards);
    void EnqueueCommand(const SentinelCommand& command);
    void InitShards(const std::vector<std::string>& shards, std::vector<std::shared_ptr<Shard>>& shard_objects);

    void ProcessWaitingCommands();

    Password GetPassword();

    Sentinel& sentinel_obj_;
    engine::ev::ThreadControl ev_thread_;

    const std::string shard_group_name_;
    const std::shared_ptr<const std::vector<std::string>> init_shards_;
    std::vector<std::unique_ptr<ConnectedStatus>> connected_statuses_;
    const std::vector<ConnectionInfo> conns_;

    const std::shared_ptr<engine::ev::ThreadPool> redis_thread_pool_;
    ev_async watch_state_{};
    ev_async watch_update_{};
    ev_async watch_create_{};
    ev_timer check_timer_{};
    mutable std::mutex sentinels_mutex_;
    std::vector<std::shared_ptr<Shard>> master_shards_;  // TODO rename
    ConnInfoByShard master_shards_info_;
    ConnInfoByShard slaves_shards_info_;
    std::shared_ptr<Shard> sentinels_;
    std::map<std::string, size_t> shards_;
    ShardInfo shard_info_;
    const std::string client_name_;
    concurrent::Variable<Password, std::mutex> password_{std::string()};
    ConnectionSecurity connection_security_;
    const double check_interval_;
    std::vector<SentinelCommand> commands_;
    std::mutex command_mutex_;
    const std::unique_ptr<KeyShard> key_shard_;
    SentinelStatisticsInternal statistics_internal_;
    std::optional<CommandsBufferingSettings> commands_buffering_settings_;
    dynamic_config::Source dynamic_config_source_;
    std::atomic<int> publish_shard_{0};
    const std::size_t database_index_{0};
};

}  // namespace storages::redis::impl

USERVER_NAMESPACE_END
