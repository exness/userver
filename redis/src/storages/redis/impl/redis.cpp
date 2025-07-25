#include <storages/redis/impl/redis.hpp>

#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <hiredis/adapters/libev.h>
#include <hiredis/hiredis.h>
#ifdef USERVER_FEATURE_REDIS_TLS
#include <hiredis/hiredis_ssl.h>
#endif

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <userver/logging/level.hpp>
#include <userver/logging/log.hpp>
#include <userver/utils/assert.hpp>
#include <userver/utils/impl/userver_experiments.hpp>
#include <userver/utils/retry_budget.hpp>
#include <userver/utils/scope_guard.hpp>
#include <userver/utils/swappingsmart.hpp>

#include <storages/redis/impl/command.hpp>
#include <storages/redis/impl/ev_wrapper.hpp>
#include <storages/redis/impl/redis_info.hpp>
#include <storages/redis/impl/redis_stats.hpp>
#include <storages/redis/impl/tcp_socket.hpp>
#include <userver/storages/redis/reply.hpp>

#include "command_control_impl.hpp"

USERVER_NAMESPACE_BEGIN

namespace storages::redis::impl {
namespace {

const auto kPingLatencyExp = 0.7;
const auto kInitialPingLatencyMs = 1000;
const size_t kMissedPingStreakThresholdDefault = 3;

// required for libhiredis < 1.0.0
#ifndef REDIS_ERR_TIMEOUT
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define REDIS_ERR_TIMEOUT 6
#endif

ReplyStatus NativeToReplyStatus(int status) {
    constexpr utils::TrivialBiMap error_map = [](auto selector) {
        return selector()
            .Case(REDIS_ERR, ReplyStatus::kOtherError)
            .Case(REDIS_OK, ReplyStatus::kOk)
            .Case(REDIS_ERR_IO, ReplyStatus::kInputOutputError)
            .Case(REDIS_ERR_OTHER, ReplyStatus::kOtherError)
            .Case(REDIS_ERR_EOF, ReplyStatus::kEndOfFileError)
            .Case(REDIS_ERR_PROTOCOL, ReplyStatus::kProtocolError)
            .Case(REDIS_ERR_OOM, ReplyStatus::kOutOfMemoryError)
            .Case(REDIS_ERR_TIMEOUT, ReplyStatus::kTimeoutError);
    };
    const auto reply_status = error_map.TryFindByFirst(status);
    if (!reply_status) {
        LOG_LIMITED_WARNING() << "Unsupported reply status=" << status;
        return ReplyStatus::kOtherError;
    }
    return *reply_status;
}

bool IsFinalState(Redis::State state) {
    return state == Redis::State::kDisconnected || state == Redis::State::kDisconnectError;
}

bool IsUnsubscribeReply(const ReplyPtr& reply) {
    if (!reply->data || !reply->data.IsArray()) return false;
    const auto& reply_array = reply->data.GetArray();
    if (reply_array.size() != 3 || !reply_array[0].IsString()) return false;
    return !strcasecmp(reply_array[0].GetString().c_str(), "UNSUBSCRIBE") ||
           !strcasecmp(reply_array[0].GetString().c_str(), "PUNSUBSCRIBE") ||
           !strcasecmp(reply_array[0].GetString().c_str(), "SUNSUBSCRIBE");
}

#ifdef USERVER_FEATURE_REDIS_TLS
struct SSLContextDeleter final {
    void operator()(redisSSLContext* ptr) const noexcept { redisFreeSSLContext(ptr); }
};
using SSLContextPtr = std::unique_ptr<redisSSLContext, SSLContextDeleter>;
#endif

}  // namespace

class Redis::RedisImpl : public std::enable_shared_from_this<Redis::RedisImpl> {
public:
    using State = Redis::State;

    RedisImpl(
        const std::shared_ptr<engine::ev::ThreadPool>& thread_pool,
        const engine::ev::ThreadControl& thread_control,
        Redis& redis_obj,
        const RedisCreationSettings& redis_settings
    );
    ~RedisImpl();

    void Connect(
        const ConnectionInfo::HostVector& host_addrs,
        int port,
        const Password& password,
        std::size_t database_index
    );
    void Disconnect();

    bool AsyncCommand(const CommandPtr& command);

    static logging::Level StateChangeToLogLevel(State old_state, State new_state);
    State GetState() const;
    const std::string& GetServer() const { return server_; }
    const std::string& GetHost() const { return host_; }
    uint16_t GetPort() const { return port_; }
    const Statistics& GetStatistics() const { return statistics_; }
    ServerId GetServerId() const { return server_id_; }
    size_t GetRunningCommands() const;
    bool IsDestroying() const { return destroying_; }
    bool IsSyncing() const { return is_syncing_; }
    bool IsAvailable() const { return GetState() == Redis::State::kConnected && !IsDestroying() && !IsSyncing(); }
    bool CanRetry() const;
    std::chrono::milliseconds GetPingLatency() const { return std::chrono::milliseconds(ping_latency_ms_); }
    void SetCommandsBufferingSettings(CommandsBufferingSettings commands_buffering_settings);
    void SetReplicationMonitoringSettings(const ReplicationMonitoringSettings& replication_monitoring_settings);
    void SetRetryBudgetSettings(const utils::RetryBudgetSettings& settings);

    void ResetRedisObj() { redis_obj_ = nullptr; }

private:
    struct SingleCommand {
        std::string cmd;
        CommandPtr meta;
        ev_timer timer{};
        std::shared_ptr<RedisImpl> redis_impl;
        bool invoke_disabled = false;
    };

    void DoDisconnect();
    void Attach();
    void Detach();

    static void OnNewCommand(struct ev_loop* loop, ev_async* w, int revents) noexcept;
    static void CommandLoopOnTimer(struct ev_loop* loop, ev_timer* w, int revents) noexcept;
    static void OnRedisReply(redisAsyncContext* c, void* r, void* privdata) noexcept;
    static void OnConnect(const redisAsyncContext* c, int status) noexcept;
    static void OnDisconnect(const redisAsyncContext* c, int status) noexcept;
    static void OnTimerPing(struct ev_loop* loop, ev_timer* w, int revents) noexcept;
    static void OnTimerInfo(struct ev_loop* loop, ev_timer* w, int revents) noexcept;
    static void OnConnectTimeout(struct ev_loop* loop, ev_timer* w, int revents) noexcept;
    static void OnCommandTimeout(struct ev_loop* loop, ev_timer* w, int revents) noexcept;

    void OnConnectImpl(int status);
    void OnDisconnectImpl(int status);
    bool InitSecureConnection();
    void InvokeCommand(const CommandPtr& command, ReplyPtr&& reply);
    void InvokeCommandError(
        const CommandPtr& command,
        const std::string& name,
        ReplyStatus status,
        std::string&& error_info
    );

    void OnNewCommandImpl();
    void CommandLoopImpl();
    void OnRedisReplyImpl(redisReply* redis_reply, void* privdata, int status, const char* errstr);
    void AccountPingLatency(std::chrono::milliseconds latency);
    void AccountRtt();
    void OnTimerPingImpl();
    void OnTimerInfoImpl();
    void SendSubscriberPing();
    void SendPing();

    void OnConnectTimeoutImpl();
    void OnCommandTimeoutImpl(ev_timer* w);

    void SetState(State state);
    void ProcessCommand(const CommandPtr& command);

    void Authenticate();
    void SelectDatabase();
    void SendReadOnly();
    void FreeCommands();

    static void LogSocketErrorReply(const CommandPtr& command, const ReplyPtr& reply);
    static void LogInstanceErrorReply(const CommandPtr& command, const ReplyPtr& reply);

    bool SetDestroying() {
        const std::lock_guard<std::mutex> lock(command_mutex_);
        if (destroying_) return false;
        destroying_ = true;
        return true;
    }

    static bool WatchCommandTimerEnabled(const CommandsBufferingSettings& commands_buffering_settings);

    bool Connect(const std::string& host, int port, const Password& password, size_t database_index);

    Redis* redis_obj_;
    engine::ev::ThreadControl ev_thread_control_;

    // Count references to thread pool in redis for correct thread pool stopping
    std::shared_ptr<engine::ev::ThreadPool> thread_pool_;

    std::mutex command_mutex_;
    std::deque<CommandPtr> commands_;
    std::atomic<bool> destroying_{false};

    redisAsyncContext* context_ = nullptr;
#ifdef USERVER_FEATURE_REDIS_TLS
    SSLContextPtr ssl_context_;
#endif
    std::atomic<State> state_{State::kInit};
    std::string host_;
    uint16_t port_ = 0;
    std::string server_;
    Password password_{std::string()};
    std::size_t database_index_ = 0;
    std::atomic<size_t> commands_size_ = 0;
    size_t sent_count_ = 0;
    size_t cmd_counter_ = 0;
    std::unordered_map<size_t, std::unique_ptr<SingleCommand>> reply_privdata_;
    std::unordered_map<const ev_timer*, size_t> reply_privdata_rev_;
    bool subscriber_ = false;
    bool is_ping_in_flight_ = false;
    std::atomic_bool is_syncing_ = false;
    size_t missed_ping_streak_{0};
    size_t missed_ping_streak_threshold_{kMissedPingStreakThresholdDefault};
    ev_timer connect_timer_{};
    ev_timer ping_timer_{};
    ev_timer info_timer_{};
    ev_timer watch_command_timer_{};
    ev_async watch_command_{};
    utils::SwappingSmart<CommandsBufferingSettings> commands_buffering_settings_;
    std::atomic_bool enable_replication_monitoring_ = false;
    std::atomic_bool forbid_requests_to_syncing_replicas_ = false;
    const bool send_readonly_;
    const ConnectionSecurity connection_security_;
    std::chrono::milliseconds ping_interval_{2000};
    std::chrono::milliseconds ping_timeout_{4000};
    std::chrono::milliseconds info_replication_interval_{2000};
    std::atomic<double> ping_latency_ms_{kInitialPingLatencyMs};
    logging::LogExtra log_extra_;
    bool watch_command_timer_started_ = false;
    Statistics statistics_;
    ServerId server_id_;
    bool attached_ = false;
    std::shared_ptr<RedisImpl> self_;
    utils::RetryBudget retry_budget_;
};

std::string_view StateToString(RedisState state) {
    constexpr utils::TrivialBiMap states_map = [](auto selector) {
        return selector()
            .Case(RedisState::kInit, "init")
            .Case(RedisState::kInitError, "init_error")
            .Case(RedisState::kConnected, "connected")
            .Case(RedisState::kDisconnecting, "disconnecting")
            .Case(RedisState::kDisconnected, "disconnected")
            .Case(RedisState::kDisconnectError, "disconnect_error");
    };

    const auto state_str = states_map.TryFind(state);
    return state_str ? *state_str : "unknown";
}

Redis::Redis(const std::shared_ptr<engine::ev::ThreadPool>& thread_pool, const RedisCreationSettings& redis_settings)
    : thread_control_(thread_pool->NextThread()) {
    impl_ = std::make_shared<RedisImpl>(thread_pool, thread_control_, *this, redis_settings);
}

Redis::~Redis() {
    thread_control_.RunInEvLoopBlocking([this]() {
        impl_->Disconnect();
        impl_->ResetRedisObj();
        impl_.reset();
    });
}

void Redis::Connect(
    const ConnectionInfo::HostVector& host_addrs,
    int port,
    const Password& password,
    size_t database_index
) {
    impl_->Connect(host_addrs, port, password, database_index);
}

bool Redis::AsyncCommand(const CommandPtr& command) { return impl_->AsyncCommand(command); }

Redis::State Redis::GetState() const { return impl_->GetState(); }

const Statistics& Redis::GetStatistics() const { return impl_->GetStatistics(); }

ServerId Redis::GetServerId() const { return impl_->GetServerId(); }

size_t Redis::GetRunningCommands() const { return impl_->GetRunningCommands(); }

std::chrono::milliseconds Redis::GetPingLatency() const { return impl_->GetPingLatency(); }

bool Redis::IsDestroying() const { return impl_->IsDestroying(); }

bool Redis::IsSyncing() const { return impl_->IsSyncing(); }

bool Redis::IsAvailable() const { return impl_->IsAvailable(); }

bool Redis::CanRetry() const { return impl_->CanRetry(); }

std::string Redis::GetServerHost() const { return impl_->GetHost(); }

uint16_t Redis::GetServerPort() const { return impl_->GetPort(); }

void Redis::SetCommandsBufferingSettings(CommandsBufferingSettings commands_buffering_settings) {
    impl_->SetCommandsBufferingSettings(commands_buffering_settings);
}

void Redis::SetRetryBudgetSettings(const utils::RetryBudgetSettings& settings) {
    impl_->SetRetryBudgetSettings(settings);
}

void Redis::SetReplicationMonitoringSettings(const ReplicationMonitoringSettings& replication_monitoring_settings) {
    impl_->SetReplicationMonitoringSettings(replication_monitoring_settings);
}

Redis::RedisImpl::RedisImpl(
    const std::shared_ptr<engine::ev::ThreadPool>& thread_pool,
    const engine::ev::ThreadControl& thread_control,
    Redis& redis_obj,
    const RedisCreationSettings& redis_settings
)
    : redis_obj_(&redis_obj),
      ev_thread_control_(thread_control),
      thread_pool_(thread_pool),
      send_readonly_(redis_settings.send_readonly),
      connection_security_(redis_settings.connection_security),
      server_id_(ServerId::Generate()),
      retry_budget_(utils::RetryBudgetSettings{100, 0.1, false}) {
    SetCommandsBufferingSettings(CommandsBufferingSettings{});
    LOG_DEBUG() << "RedisImpl() server_id=" << GetServerId().GetId();
}

Redis::RedisImpl::~RedisImpl() {
    LOG_DEBUG() << "~RedisImpl() server_id=" << GetServerId().GetId() << " server=" << GetServer();
    server_id_.RemoveDescription();
}

void Redis::RedisImpl::Attach() {
    connect_timer_.data = this;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
    ev_timer_init(&connect_timer_, OnConnectTimeout, ToEvDuration(ping_interval_), 0.0);
    ev_thread_control_.Start(connect_timer_);

    // start after connecting
    watch_command_.data = this;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
    ev_async_init(&watch_command_, OnNewCommand);

    watch_command_timer_.data = this;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
    ev_timer_init(&watch_command_timer_, CommandLoopOnTimer, 0.0, 0.0);

    ping_timer_.data = this;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
    ev_timer_init(&ping_timer_, OnTimerPing, 0.0, 0.0);

    info_timer_.data = this;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
    ev_timer_init(&info_timer_, OnTimerInfo, 0.0, 0.0);

    attached_ = true;
}

void Redis::RedisImpl::Detach() {
    if (!attached_) return;

    ev_thread_control_.Stop(watch_command_);
    ev_thread_control_.Stop(watch_command_timer_);
    ev_thread_control_.Stop(ping_timer_);
    ev_thread_control_.Stop(info_timer_);
    ev_thread_control_.Stop(connect_timer_);

    attached_ = false;
}

void Redis::RedisImpl::Connect(
    const ConnectionInfo::HostVector& host_addrs,
    int port,
    const Password& password,
    size_t database_index
) {
    for (const auto& host : host_addrs)
        if (Connect(host, port, password, database_index)) return;

    LOG_ERROR() << "error async connect to Redis server (host addrs =" << host_addrs << ", port=" << port << ")";
    SetState(State::kInitError);
}

bool Redis::RedisImpl::Connect(const std::string& host, int port, const Password& password, size_t database_index) {
    UASSERT(context_ == nullptr);
    UASSERT(state_ == State::kInit);

    server_ = host + ":" + std::to_string(port);
    server_id_.SetDescription(server_);
    host_ = host;
    port_ = port;
    log_extra_.Extend("redis_server", GetServer());
    log_extra_.Extend("server_id", GetServerId().GetId());
    password_ = password;
    database_index_ = database_index;
    LOG_INFO() << log_extra_ << "Async connect to Redis server=" << GetServer();
    context_ = redisAsyncConnect(host.c_str(), port);

    UASSERT(context_ != nullptr);

    context_->data = this;

    if (context_->err) {
        LOG_WARNING() << "error after redisAsyncConnect (host=" << host << ", port=" << port
                      << "): " << context_->errstr;
        redisAsyncFree(context_);
        context_ = nullptr;
        return false;
    }

    ev_thread_control_.RunInEvLoopBlocking([this, &host]() {
        bool err = false;
        auto CheckError = [&err, &host](int status, const std::string& name) {
            if (status != REDIS_OK) {
                err = true;
                LOG_ERROR() << "error in " << name << " with host " << host;
            }
        };
        if (!err) Attach();
        if (!err) CheckError(redisLibevAttach(ev_thread_control_.GetEvLoop(), context_), "redisLibevAttach");
        if (!err) CheckError(redisAsyncSetConnectCallback(context_, OnConnect), "redisAsyncSetConnectCallback");
        if (!err)
            CheckError(redisAsyncSetDisconnectCallback(context_, OnDisconnect), "redisAsyncSetDisconnectCallback");
        SetState(err ? State::kInitError : State::kInit);
    });
    return true;
}

void Redis::RedisImpl::Disconnect() {
    auto self = shared_from_this();  // prevents deleting this in FreeCommands()
    if (!SetDestroying()) return;
    ev_thread_control_.RunInEvLoopBlocking([this] { DoDisconnect(); });
}

void Redis::RedisImpl::DoDisconnect() {
    Detach();

    if (state_ == State::kInit || state_ == State::kConnected) redisAsyncDisconnect(context_);

    FreeCommands();

    if (state_ == State::kInit) {
        /*
         * redisAsyncDisconnect() above doesn't call OnDisconnect() at all
         * as we haven't connected, we have to SetState by ourselves.
         */
        SetState(State::kDisconnectError);
    }

    if (!IsFinalState(state_)) {
        SetState(State::kDisconnecting);
    }
}

void Redis::RedisImpl::InvokeCommand(const CommandPtr& command, ReplyPtr&& reply) {
    UASSERT(reply);

    const CommandControlImpl cc{command->control};
    if (cc.account_in_statistics) statistics_.AccountReplyReceived(reply, command);
    reply->server = server_;
    if (reply->status == ReplyStatus::kTimeoutError) {
        reply->log_extra.Extend("timeout_ms", cc.timeout_single.count());
        retry_budget_.AccountFail();
    }
    if (reply->status == ReplyStatus::kOk) {
        retry_budget_.AccountOk();
    }

    reply->server_id = server_id_;
    reply->log_extra.Extend("redis_server", server_);
    reply->log_extra.Extend("reply_status", ToString(reply->status));

    if (reply->IsLoggableError()) LogSocketErrorReply(command, reply);

    bool need_disconnect = false;
    if (reply->IsUnusableInstanceError() || reply->IsReadonlyError()) {
        LogInstanceErrorReply(command, reply);
        need_disconnect = true;
    }

    try {
        command->callback(command, reply);
    } catch (const std::exception& ex) {
        LOG_WARNING() << "exception in callback handler (" << command->args << ") " << ex;
    }

    if (need_disconnect) Disconnect();
}

void Redis::RedisImpl::InvokeCommandError(
    const CommandPtr& command,
    const std::string& name,
    ReplyStatus status,
    std::string&& error_info
) {
    UASSERT(status != ReplyStatus::kOk);
    InvokeCommand(command, std::make_shared<Reply>(name, ReplyData::CreateError(std::move(error_info)), status));
}

void Redis::RedisImpl::LogSocketErrorReply(const CommandPtr& command, const ReplyPtr& reply) {
    LOG_WARNING() << "Request to Redis server " << reply->server << " failed with status " << reply->status << " ("
                  << reply->GetStatusString() << ")" << reply->GetLogExtra() << command->GetLogExtra();
}

void Redis::RedisImpl::LogInstanceErrorReply(const CommandPtr& command, const ReplyPtr& reply) {
    LOG_ERROR() << "Request to Redis server " << reply->server
                << " failed with Redis error reply: " << reply->data.ToDebugString() << reply->GetLogExtra()
                << command->GetLogExtra();
}

bool Redis::RedisImpl::WatchCommandTimerEnabled(const CommandsBufferingSettings& commands_buffering_settings) {
    return commands_buffering_settings.buffering_enabled &&
           commands_buffering_settings.watch_command_timer_interval != std::chrono::microseconds::zero();
}

bool Redis::RedisImpl::AsyncCommand(const CommandPtr& command) {
    LOG_DEBUG() << "AsyncCommand for server_id=" << GetServerId().GetId()
                << " server=" << GetServerId().GetDescription() << " cmd=" << command->args;
    {
        const std::lock_guard<std::mutex> lock(command_mutex_);
        if (destroying_) return false;
        ++commands_size_;
        commands_.push_back(command);
    }
    ev_thread_control_.Send(watch_command_);
    return true;
}

void Redis::RedisImpl::OnTimerPing(struct ev_loop*, ev_timer* w, int) noexcept {
    auto* impl = static_cast<Redis::RedisImpl*>(w->data);
    UASSERT(impl != nullptr);
    try {
        impl->OnTimerPingImpl();
    } catch (const std::exception& ex) {
        LOG_ERROR() << "OnTimerPingImpl() failed: " << ex;
    }
}

void Redis::RedisImpl::OnTimerInfo(struct ev_loop*, ev_timer* w, int) noexcept {
    auto* impl = static_cast<Redis::RedisImpl*>(w->data);
    UASSERT(impl != nullptr);
    try {
        impl->OnTimerInfoImpl();
    } catch (const std::exception& ex) {
        LOG_ERROR() << "OnTimerInfoImpl() failed: " << ex;
    }
}

void Redis::RedisImpl::OnCommandTimeout(struct ev_loop*, ev_timer* w, int) noexcept {
    auto* impl = static_cast<Redis::RedisImpl*>(w->data);
    UASSERT(impl != nullptr);
    try {
        impl->OnCommandTimeoutImpl(w);
    } catch (const std::exception& ex) {
        LOG_ERROR() << "OnCommandTimeoutImpl() failed: " << ex;
    }
}

void Redis::RedisImpl::OnCommandTimeoutImpl(ev_timer* w) {
    const size_t cmd_idx = reply_privdata_rev_.at(w);
    auto reply_iterator = reply_privdata_.find(cmd_idx);
    if (reply_iterator != reply_privdata_.end()) {
        SingleCommand& command = *reply_iterator->second;
        UASSERT(reply_privdata_rev_.count(&command.timer));
        UASSERT(w == &command.timer);
        reply_privdata_rev_.erase(&command.timer);
        command.invoke_disabled = true;
        InvokeCommandError(command.meta, command.cmd, ReplyStatus::kTimeoutError, "Command timeout");
    }
}

void Redis::RedisImpl::AccountPingLatency(std::chrono::milliseconds latency) {
    statistics_.AccountPing(latency);
    ping_latency_ms_ = (ping_latency_ms_.load() * kPingLatencyExp + latency.count() * (1 - kPingLatencyExp));
    logging::LogExtra log_extra = log_extra_;
    log_extra.Extend("ping_ms", latency.count());
    log_extra.Extend("stat_ms", ping_latency_ms_.load());
    LOG_DEBUG() << "Got ping for Redis server: " << latency.count() << "ms, current ping stat is "
                << ping_latency_ms_.load() << "ms" << log_extra;
}

void Redis::RedisImpl::AccountRtt() {
    auto rtt = GetSocketPeerRtt(context_->c.fd);
    if (rtt) {
        AccountPingLatency(std::chrono::duration_cast<std::chrono::milliseconds>(*rtt));
    }
}

inline void Redis::RedisImpl::OnTimerPingImpl() {
    ev_thread_control_.Stop(ping_timer_);

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
    ev_timer_set(&ping_timer_, ToEvDuration(ping_interval_), 0.0);
    ev_thread_control_.Start(ping_timer_);

    AccountRtt();
    if (is_ping_in_flight_) {
        if (++missed_ping_streak_ >= missed_ping_streak_threshold_) {
            Disconnect();
        }
        return;
    } else {
        missed_ping_streak_ = 0;
    }

    if (subscriber_) {
        SendSubscriberPing();
    } else {
        SendPing();
    }
}

inline void Redis::RedisImpl::OnTimerInfoImpl() {
    ev_thread_control_.Stop(info_timer_);

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
    ev_timer_set(&info_timer_, ToEvDuration(info_replication_interval_), 0.0);
    ev_thread_control_.Start(info_timer_);

    if (!enable_replication_monitoring_.load(std::memory_order_relaxed)) {
        /// pretend we never syncing
        is_syncing_ = false;
        return;
    }

    CommandControl cc{ping_timeout_, ping_timeout_, 1};
    cc.account_in_statistics = false;

    ProcessCommand(PrepareCommand(
        CmdArgs{"INFO", "REPLICATION"},
        [this](const CommandPtr&, ReplyPtr reply) {
            if (!*reply) {
                LOG_DEBUG() << "Failed to get INFO for server_id=" << GetServerId().GetId() << ", host=" << GetHost();
                return;
            }
            if (!reply->data.IsString()) {
                LOG_DEBUG() << "Failed to get INFO for server_id=" << GetServerId().GetId() << ", host=" << GetHost()
                            << ". reply data is not an string but " << reply->data.GetTypeString();
                return;
            }
            const auto& value = reply->data.GetString();
            const auto redis_info = ParseReplicationInfo(value);
            is_syncing_ = forbid_requests_to_syncing_replicas_.load(std::memory_order_relaxed) && redis_info.is_syncing;
            statistics_.is_syncing = redis_info.is_syncing;
            statistics_.offset_from_master_bytes = redis_info.slave_read_repl_offset - redis_info.slave_repl_offset;
        },
        cc
    ));
}

void Redis::RedisImpl::SendSubscriberPing() {
    CommandControl cc{ping_timeout_, ping_timeout_, 1};
    cc.account_in_statistics = false;

    is_ping_in_flight_ = true;
    ProcessCommand(PrepareCommand(
        CmdArgs{"SUBSCRIBE", CmdWithArgs::kSubscriberPingChannelName},
        [this](const CommandPtr&, ReplyPtr reply) {
            if (!*reply || !reply->data.IsArray()) {
                Disconnect();
                return;
            }
            const auto& reply_array = reply->data.GetArray();
            if (reply_array.size() != 3 || !reply_array[0].IsString()) {
                Disconnect();
                return;
            }
            if (!strcasecmp(reply_array[0].GetString().c_str(), "SUBSCRIBE")) {
                ProcessCommand(
                    PrepareCommand(CmdArgs{"UNSUBSCRIBE", CmdWithArgs::kSubscriberPingChannelName}, ReplyCallback{})
                );
            } else if (!strcasecmp(reply_array[0].GetString().c_str(), "UNSUBSCRIBE")) {
                is_ping_in_flight_ = false;
            }
        },
        cc
    ));
}

void Redis::RedisImpl::SendPing() {
    CommandControl cc{ping_timeout_ /*timeout_single*/, ping_timeout_ /*timeout_all*/, 1 /*max_retries*/};
    cc.account_in_statistics = false;

    is_ping_in_flight_ = true;
    ProcessCommand(PrepareCommand(
        CmdArgs{"PING"},
        [this](const CommandPtr&, ReplyPtr reply) {
            is_ping_in_flight_ = false;
            if (!*reply || !reply->data.IsStatus()) {
                Disconnect();
            }
        },
        cc
    ));
}

void Redis::RedisImpl::OnConnectTimeout(struct ev_loop*, ev_timer* w, int) noexcept {
    auto* impl = static_cast<Redis::RedisImpl*>(w->data);
    UASSERT(impl != nullptr);
    try {
        impl->OnConnectTimeoutImpl();
    } catch (const std::exception& ex) {
        LOG_ERROR() << "OnConnectTimeoutImpl() failed: " << ex;
    }
}

void Redis::RedisImpl::OnConnectTimeoutImpl() {
    ev_thread_control_.Stop(connect_timer_);

    LOG_WARNING() << "connect() to redis timeouted, server_id=" << GetServerId().GetId() << " server=" << GetServer();
    Disconnect();
}

Redis::State Redis::RedisImpl::GetState() const { return state_; }

size_t Redis::RedisImpl::GetRunningCommands() const { return sent_count_; }

logging::Level Redis::RedisImpl::StateChangeToLogLevel(State /*old_state*/, State new_state) {
    switch (new_state) {
        case State::kConnected:
        case State::kInit:
        case State::kDisconnecting:
        case State::kDisconnected:
            return logging::Level::kInfo;

        case State::kInitError:
        case State::kDisconnectError:
            return logging::Level::kWarning;
    }
    return logging::Level::kWarning;
}

void Redis::RedisImpl::SetState(State state) {
    if (state == state_) return;

    if (IsFinalState(state_)) {
        LOG_INFO() << "skipped SetState() from " << StateToString(state_) << " to " << StateToString(state);
        return;
    }
    LOG(StateChangeToLogLevel(state_, state))
        << log_extra_ << "Redis server connection state for server=" << GetServer()
        << " (server_id=" << GetServerId().GetId() << ") changed from " << StateToString(state_) << " to "
        << StateToString(state);
    state_ = state;
    statistics_.AccountStateChanged(state);

    auto self = shared_from_this();  // prevents deleting this in Disconnect()
    if (state == State::kConnected) {
        ev_thread_control_.RunInEvLoopBlocking([this] {
            ev_thread_control_.Start(watch_command_);
            ev_thread_control_.Start(ping_timer_);
            ev_thread_control_.Start(info_timer_);
        });
    } else if (state == State::kInitError || state == State::kDisconnectError || state == State::kDisconnected)
        Disconnect();

    if (redis_obj_) redis_obj_->signal_state_change(state);
}

void Redis::RedisImpl::FreeCommands() {
    while (!commands_.empty()) {
        auto command = commands_.front();
        commands_.pop_front();
        --commands_size_;
        for (const auto& args : command->args) {
            InvokeCommandError(
                command,
                args.GetCommandName(),
                ReplyStatus::kEndOfFileError,
                "Disconnecting, killing commands still waiting in send queue"
            );
        }
    }

    for (auto& info : reply_privdata_) {
        ev_thread_control_.Stop(info.second->timer);
        if (!info.second->invoke_disabled) {
            info.second->invoke_disabled = true;
            InvokeCommandError(
                info.second->meta,
                info.second->cmd,
                ReplyStatus::kEndOfFileError,
                "Disconnecting, killing commands still waiting for reply"
            );
        }
    }
    reply_privdata_.clear();
}

void Redis::RedisImpl::OnNewCommand(struct ev_loop*, ev_async* w, int) noexcept {
    auto* impl = static_cast<Redis::RedisImpl*>(w->data);
    UASSERT(impl != nullptr);
    try {
        impl->OnNewCommandImpl();
    } catch (const std::exception& ex) {
        LOG_ERROR() << "CommandLoopImpl() failed: " << ex.what();
    }
}

void Redis::RedisImpl::OnNewCommandImpl() {
    auto commands_buffering_settings = commands_buffering_settings_.Get();
    if (WatchCommandTimerEnabled(*commands_buffering_settings) &&
        (!commands_buffering_settings->commands_buffering_threshold ||
         commands_size_.load() < commands_buffering_settings->commands_buffering_threshold)) {
        if (!std::exchange(watch_command_timer_started_, true)) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
            ev_timer_set(
                &watch_command_timer_, ToEvDuration(commands_buffering_settings->watch_command_timer_interval), 0.0
            );
            ev_thread_control_.Start(watch_command_timer_);
        }
    } else {
        CommandLoopImpl();
    }
}

void Redis::RedisImpl::CommandLoopOnTimer(struct ev_loop*, ev_timer* w, int) noexcept {
    auto* impl = static_cast<Redis::RedisImpl*>(w->data);
    UASSERT(impl != nullptr);
    try {
        impl->CommandLoopImpl();
    } catch (const std::exception& ex) {
        LOG_ERROR() << "CommandLoopImpl() failed: " << ex;
    }
}

void Redis::RedisImpl::CommandLoopImpl() {
    if (WatchCommandTimerEnabled(*commands_buffering_settings_.Get())) {
        if (std::exchange(watch_command_timer_started_, false)) {
            ev_thread_control_.Stop(watch_command_timer_);
        }
    }
    std::deque<CommandPtr> commands;
    {
        const std::lock_guard<std::mutex> lock(command_mutex_);
        commands_size_ -= commands_.size();
        std::swap(commands_, commands);
    }
    LOG_TRACE() << "commands size=" << commands.size();
    for (auto& command : commands) {
        ProcessCommand(command);
    }
}

void Redis::RedisImpl::OnConnect(const redisAsyncContext* c, int status) noexcept {
    auto* impl = static_cast<Redis::RedisImpl*>(c->data);
    UASSERT(impl != nullptr);
    try {
        impl->OnConnectImpl(status);
    } catch (const std::exception& ex) {
        LOG_ERROR() << "OnConnectImpl() failed: " << ex;
    }
}

void Redis::RedisImpl::OnDisconnect(const redisAsyncContext* c, int status) noexcept {
    auto* impl = static_cast<Redis::RedisImpl*>(c->data);
    UASSERT(impl != nullptr);
    try {
        impl->OnDisconnectImpl(status);
    } catch (const std::exception& ex) {
        LOG_ERROR() << "OnDisconnectImpl() failed: " << ex;
    }
}

void Redis::RedisImpl::OnConnectImpl(int status) {
    ev_thread_control_.Stop(connect_timer_);

    if (status != REDIS_OK) {
        LOG_WARNING() << log_extra_ << "Connect to Redis failed. Status=" << status << ". Hiredis errstr='"
                      << (status == REDIS_ERR ? context_->errstr : "") << '\'';
        SetState(State::kDisconnected);
        return;
    }

    if (connection_security_ == ConnectionSecurity::kTLS && !InitSecureConnection()) {
        Disconnect();
        return;
    }

    LOG_INFO() << log_extra_ << "Connected to Redis successfully";
    self_ = shared_from_this();

    const int keep_alive_status = redisEnableKeepAlive(&context_->c);
    if (keep_alive_status != REDIS_OK) {
        LOG_ERROR() << "redisEnableKeepAlive() failed. Hiredis errstr='"
                    << (keep_alive_status == REDIS_ERR ? context_->errstr : "") << '\'';
        Disconnect();
        return;
    }

    Authenticate();
}

void Redis::RedisImpl::OnDisconnectImpl(int status) {
    if (status == REDIS_ERR) {
        LOG_LIMITED_WARNING() << "Got disconnect error from hiredis (" << context_->errstr
                              << "). For more information look in server logs ("
                                 "https://wiki.yandex-team.ru/taxi/backend/userver/redis/"
                                 "#logiservera).";
    }
    SetState(status == REDIS_OK ? State::kDisconnected : State::kDisconnectError);
    context_ = nullptr;
    self_.reset();
}

bool Redis::RedisImpl::InitSecureConnection() {
#ifdef USERVER_FEATURE_REDIS_TLS
    if (!ssl_context_) {
        redisSSLContextError ssl_error{};
        ssl_context_.reset(redisCreateSSLContext(nullptr, nullptr, nullptr, nullptr, nullptr, &ssl_error));
        if (!ssl_context_) {
            LOG_ERROR() << "redisCreateSSLContext failed: " << redisSSLContextGetError(ssl_error);
            return false;
        }
    }

    if (redisInitiateSSLWithContext(&context_->c, ssl_context_.get()) != REDIS_OK) {
        LOG_ERROR() << "redisInitiateSSLWithContext failed. Hiredis errstr='" << context_->errstr << '\''
                    << " server=" << server_;
        return false;
    }

    return true;
#else
    LOG_ERROR() << log_extra_ << "SSL/TLS connections are not supported";
    return false;
#endif
}

void Redis::RedisImpl::Authenticate() {
    if (password_.GetUnderlying().empty()) {
        SendReadOnly();
    } else {
        ProcessCommand(PrepareCommand(
            CmdArgs{"AUTH", password_.GetUnderlying()},
            [this](const CommandPtr&, ReplyPtr reply) {
                if (*reply && reply->data.IsStatus()) {
                    SendReadOnly();
                } else {
                    if (*reply) {
                        if (reply->IsUnknownCommandError()) {
                            LOG_WARNING() << log_extra_
                                          << "AUTH failed: unknown command `AUTH` - "
                                             "possible when connecting to sentinel instead "
                                             "of RedisCluster instance";
                            Disconnect();
                            return;
                        }
                        LOG_LIMITED_ERROR()
                            << log_extra_ << "AUTH failed: response type=" << reply->data.GetTypeString()
                            << " msg=" << reply->data.ToDebugString();
                    } else {
                        LOG_LIMITED_ERROR() << "AUTH failed with status " << reply->status << " ("
                                            << reply->GetStatusString() << ") " << log_extra_;
                    }
                    Disconnect();
                }
            }
        ));
    }
}

void Redis::RedisImpl::SendReadOnly() {
    if (!send_readonly_) {
        SelectDatabase();
        return;
    }

    LOG_DEBUG() << "Send READONLY command to slave " << GetServerId().GetDescription() << " in cluster mode";
    ProcessCommand(PrepareCommand(CmdArgs{"READONLY"}, [this](const CommandPtr&, ReplyPtr reply) {
        if (*reply && reply->data.IsStatus()) {
            SelectDatabase();
        } else {
            if (*reply) {
                LOG_LIMITED_ERROR() << log_extra_ << "READONLY failed: response type=" << reply->data.GetTypeString()
                                    << " msg=" << reply->data.ToDebugString();
            } else {
                LOG_LIMITED_ERROR() << "READONLY failed with status=" << reply->status << " ("
                                    << reply->GetStatusString() << ") " << log_extra_;
            }
            Disconnect();
        }
    }));
}

void Redis::RedisImpl::SelectDatabase() {
    // To get rid of the redundant `SELECT 0` command
    // since 0 is the default database index, and it will be set automatically
    if (database_index_ == 0) {
        SetState(RedisState::kConnected);
        return;
    }

    ProcessCommand(PrepareCommand(CmdArgs{"SELECT", database_index_}, [this](const CommandPtr&, ReplyPtr reply) {
        if (*reply && reply->data.IsStatus()) {
            SetState(RedisState::kConnected);
            LOG_INFO() << log_extra_ << "Selected redis logical database with index " << database_index_;
            return;
        }

        const utils::ScopeGuard auto_disconnect([this]() { Disconnect(); });

        if (!*reply) {
            LOG_LIMITED_ERROR() << "SELECT failed with status " << reply->status << " (" << reply->GetStatusString()
                                << ") " << log_extra_;
            return;
        }

        if (reply->IsUnknownCommandError()) {
            LOG_WARNING() << log_extra_
                          << "SELECT failed: unknown command `SELECT` - "
                             "possible when connecting to Sentinel instead "
                             "of Redis master or slave instance";
            return;
        }

        LOG_LIMITED_ERROR() << log_extra_ << "SELECT failed: response type=" << reply->data.GetTypeString()
                            << " msg=" << reply->data.ToDebugString();
    }));
}

void Redis::RedisImpl::OnRedisReply(redisAsyncContext* c, void* r, void* privdata) noexcept {
    auto* impl = static_cast<Redis::RedisImpl*>(c->data);
    UASSERT(impl != nullptr);
    try {
        auto* redis_reply = static_cast<redisReply*>(r);
        if (!redis_reply && c->err == REDIS_OK) {
            // redisAsyncDisconnect causes empty replies with OK status.
            // Translate to something sensible.
            impl->OnRedisReplyImpl(nullptr, privdata, REDIS_ERR_EOF, "Disconnecting");
        } else if (redis_reply && redis_reply->type == REDIS_REPLY_ERROR && c->err == REDIS_OK) {
            // redis_reply contains error that missmatch Reply status OK.
            // Fix the status here to pass the Reply UASSERT checks.
            UASSERT_MSG(
                !c->errstr || c->errstr[0] == '\0', fmt::format("For OK status there's an error string: {}", c->errstr)
            );
            impl->OnRedisReplyImpl(redis_reply, privdata, REDIS_ERR_OTHER, nullptr);
        } else {
            impl->OnRedisReplyImpl(redis_reply, privdata, c->err, c->errstr);
        }
    } catch (const std::exception& ex) {
        LOG_ERROR() << "OnRedisReplyImpl() failed: " << ex;
    }
}

void Redis::RedisImpl::OnRedisReplyImpl(redisReply* redis_reply, void* privdata, int status, const char* errstr) {
    auto data = reply_privdata_.find(reinterpret_cast<size_t>(privdata));
    if (data == reply_privdata_.end()) return;

    std::unique_ptr<SingleCommand> command_ptr;
    SingleCommand* pcommand = nullptr;

    ev_thread_control_.Stop(data->second->timer);
    pcommand = data->second.get();

    UASSERT_MSG(
        redis_reply || (errstr && errstr[0] != '\0'),
        fmt::format("Neither reply nor error string for command {}", pcommand->cmd)
    );
    UASSERT_MSG(
        !redis_reply || !errstr || errstr[0] == '\0',
        fmt::format("Reply and error string '{}' for command {}", errstr, pcommand->cmd)
    );
    auto reply = std::make_shared<Reply>(
        pcommand->cmd,
        redis_reply ? ReplyData{redis_reply} : ReplyData::CreateError(errstr),
        NativeToReplyStatus(status)
    );

    // After 'subscribe x' + 'unsubscribe x' + 'subscribe x' requests
    // 'unsubscribe' reply can be received as a reply to the second subscribe
    // request instead of the first (with 'privdata' related to second
    // request). After that 'subscribe' and 'message' replies will be received
    // as a reply to the second request. You must not send the second
    // SUBSCRIBE request with the same channel name until the response to
    // UNSUBSCRIBE request is received. shard_subscriber::Fsm checks it.
    // TODO: add check in RedisImpl.
    if (!subscriber_ || !redis_reply || IsUnsubscribeReply(reply)) {
        command_ptr = std::move(data->second);
        if (!subscriber_) --sent_count_;

        if (subscriber_) {
            LOG_DEBUG() << "server_id=" << GetServerId().GetId() << " erase privdata=" << data->first
                        << " unsub=" << IsUnsubscribeReply(reply);
        }
        if (!pcommand->invoke_disabled) {
            UASSERT(reply_privdata_rev_.count(&pcommand->timer));
            UASSERT(reply_privdata_rev_.at(&pcommand->timer) == reinterpret_cast<size_t>(privdata));
            reply_privdata_rev_.erase(&pcommand->timer);
        }

        reply_privdata_.erase(data);
    }
    if (!pcommand->invoke_disabled) {
        // prevents double unsubscribe handling
        if (subscriber_ && (!reply->IsOk() || !reply->data || !reply->data.IsArray())) pcommand->invoke_disabled = true;
        InvokeCommand(pcommand->meta, std::move(reply));
    }
}

void Redis::RedisImpl::ProcessCommand(const CommandPtr& command) {
    command->ResetStartHandlingTime();
    statistics_.AccountCommandSent(command);

    bool multi = false;
    for (const auto& args : command->args) {
        if (args.IsMultiCommand()) multi = true;

        if (!context_) {
            LOG_ERROR() << log_extra_ << "no context";
            InvokeCommandError(command, args.GetCommandName(), ReplyStatus::kOtherError, "No context");
            continue;
        }

        const bool is_special = args.IsSubscribesCommand();
        if (is_special) subscriber_ = true;
        if (subscriber_ && !is_special) {
            LOG_ERROR() << log_extra_ << "impossible for subscriber: " << args.GetCommandName();
            InvokeCommandError(command, args.GetCommandName(), ReplyStatus::kOtherError, "Impossible for subscriber");
            continue;
        }
        if (is_special && !args.IsSubscriberPingChannel()) {
            LOG_INFO() << "Process '" << fmt::to_string(args.GetJoinedArgs(" ")) << "' command" << log_extra_;
        }

        {
            static constexpr std::size_t kTopArgsCount = 8;
            boost::container::small_vector<const char*, kTopArgsCount> argv;
            boost::container::small_vector<std::size_t, kTopArgsCount> argv_len;
            args.FillPointerSizesStorages(argv, argv_len);
            const auto elements_count = argv.size();
            UASSERT(elements_count == argv_len.size());
            UASSERT(elements_count != 0);

            if (command->asking && (!multi || args.IsMultiCommand())) {
                static const char* asking = "ASKING";
                static const size_t asking_len = strlen(asking);
                redisAsyncCommandArgv(context_, nullptr, nullptr, 1, &asking, &asking_len);
            }
            if (redisAsyncCommandArgv(
                    context_,
                    OnRedisReply,
                    reinterpret_cast<void*>(cmd_counter_),
                    elements_count,
                    argv.data(),
                    argv_len.data()
                ) != REDIS_OK) {
                LOG_ERROR() << log_extra_ << "redisAsyncCommandArgv() failed on command " << args.GetCommandName();
                InvokeCommandError(command, args.GetCommandName(), ReplyStatus::kOtherError, "Failed on command");
                continue;
            }
        }

        if (args.IsExecCommand()) multi = false;

        if (!args.IsUnsubscribeCommand()) {
            auto entry = std::make_unique<SingleCommand>();
            entry->cmd = args.GetCommandName();
            entry->meta = command;
            entry->timer.data = this;
            entry->redis_impl = shared_from_this();
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
            ev_timer_init(
                &entry->timer, OnCommandTimeout, ToEvDuration(CommandControlImpl{command->control}.timeout_single), 0.0
            );
            ev_thread_control_.Start(entry->timer);

            UASSERT(!reply_privdata_rev_.count(&entry->timer));
            reply_privdata_rev_[&entry->timer] = cmd_counter_;
            [[maybe_unused]] auto cmd_iterator = reply_privdata_.emplace(cmd_counter_, std::move(entry));
            UASSERT(cmd_iterator.second);
        }

        if (!subscriber_) ++sent_count_;
        ++cmd_counter_;
    }
}

bool Redis::RedisImpl::CanRetry() const { return retry_budget_.CanRetry(); }

void Redis::RedisImpl::SetCommandsBufferingSettings(CommandsBufferingSettings commands_buffering_settings) {
    commands_buffering_settings_.Set(std::make_shared<CommandsBufferingSettings>(commands_buffering_settings));
}
void Redis::RedisImpl::SetReplicationMonitoringSettings(
    const ReplicationMonitoringSettings& replication_monitoring_settings
) {
    enable_replication_monitoring_ = replication_monitoring_settings.enable_monitoring;
    forbid_requests_to_syncing_replicas_ = replication_monitoring_settings.restrict_requests;
}

void Redis::RedisImpl::SetRetryBudgetSettings(const utils::RetryBudgetSettings& settings) {
    retry_budget_.SetSettings(settings);
}

}  // namespace storages::redis::impl

USERVER_NAMESPACE_END
