#pragma once

#include <cstring>
#include <memory>

#include <boost/signals2/signal.hpp>

#include <engine/ev/thread_control.hpp>
#include <engine/ev/thread_pool.hpp>
#include <userver/utils/retry_budget.hpp>

#include <storages/redis/impl/request.hpp>
#include <userver/storages/redis/base.hpp>
#include <userver/storages/redis/fwd.hpp>
#include <userver/storages/redis/redis_state.hpp>

#include "redis_creation_settings.hpp"

USERVER_NAMESPACE_BEGIN

namespace storages::redis::impl {

class Statistics;

class Redis {
public:
    using State = RedisState;

    Redis(const std::shared_ptr<engine::ev::ThreadPool>& thread_pool, const RedisCreationSettings& redis_settings);
    ~Redis();

    Redis(Redis&& o) = delete;

    void Connect(
        const ConnectionInfo::HostVector& host_addrs,
        int port,
        const Password& password,
        std::size_t database_index
    );

    bool AsyncCommand(const CommandPtr& command);
    size_t GetRunningCommands() const;
    std::chrono::milliseconds GetPingLatency() const;
    bool IsDestroying() const;
    std::string GetServerHost() const;
    uint16_t GetServerPort() const;
    bool IsSyncing() const;
    bool IsAvailable() const;
    bool CanRetry() const;

    State GetState() const;
    const Statistics& GetStatistics() const;
    ServerId GetServerId() const;

    void SetCommandsBufferingSettings(CommandsBufferingSettings commands_buffering_settings);
    void SetReplicationMonitoringSettings(const ReplicationMonitoringSettings& replication_monitoring_settings);
    void SetRetryBudgetSettings(const utils::RetryBudgetSettings& settings);

    // NOLINTNEXTLINE(misc-non-private-member-variables-in-classes)
    boost::signals2::signal<void(State)> signal_state_change;

private:
    class RedisImpl;
    engine::ev::ThreadControl thread_control_;
    std::shared_ptr<RedisImpl> impl_;
};

template <typename Rep, typename Period>
double ToEvDuration(const std::chrono::duration<Rep, Period>& duration) {
    return std::chrono::duration_cast<std::chrono::duration<double>>(duration).count();
}

std::string_view StateToString(RedisState state);

}  // namespace storages::redis::impl

USERVER_NAMESPACE_END
