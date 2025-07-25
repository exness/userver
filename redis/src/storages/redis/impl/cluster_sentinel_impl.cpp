#include "cluster_sentinel_impl.hpp"

#include <atomic>

#include <fmt/format.h>
#include <boost/container_hash/hash.hpp>
#include <boost/crc.hpp>

#include <userver/concurrent/variable.hpp>
#include <userver/logging/log.hpp>
#include <userver/rcu/rcu.hpp>
#include <userver/storages/redis/exception.hpp>
#include <userver/storages/redis/redis_state.hpp>
#include <userver/storages/redis/reply.hpp>
#include <userver/utils/algo.hpp>
#include <userver/utils/datetime/steady_coarse_clock.hpp>
#include <userver/utils/fast_scope_guard.hpp>
#include <userver/utils/text.hpp>

#include <engine/ev/watcher.hpp>
#include <engine/ev/watcher/async_watcher.hpp>
#include <engine/ev/watcher/periodic_watcher.hpp>
#include <storages/redis/impl/cluster_topology.hpp>
#include <storages/redis/impl/redis_connection_holder.hpp>
#include <storages/redis/impl/sentinel.hpp>
#include <storages/redis/impl/standalone_topology_holder.hpp>
#include <storages/redis/impl/topology_holder_base.hpp>

#include "command_control_impl.hpp"

USERVER_NAMESPACE_BEGIN

// https://github.com/boostorg/signals2/issues/59
// NOLINTBEGIN(clang-analyzer-cplusplus.NewDelete)

namespace storages::redis::impl {

namespace {
constexpr auto kProcessCreationInterval = std::chrono::seconds(3);
constexpr auto kDeleteNodesCheckInterval = std::chrono::seconds(60);
constexpr auto kDeleteNodeInterval = std::chrono::seconds(600);
constexpr std::size_t kClusterDatabaseIndex = 0;

bool CheckQuorum(size_t requests_sent, size_t responses_parsed) {
    const size_t quorum = requests_sent / 2 + 1;
    return responses_parsed >= quorum;
}

struct NodeAddresses {
    bool operator==(const NodeAddresses& other) const noexcept {
        return ip == other.ip && fqdn_name == other.fqdn_name;
    }

    std::string ip;
    std::optional<std::string> fqdn_name;
};

struct NodeAddressesHasher {
    std::size_t operator()(const NodeAddresses& addr) const noexcept {
        std::size_t result = 0;
        boost::hash_combine(result, addr.ip);
        if (addr.fqdn_name.has_value()) {
            boost::hash_combine(result, *addr.fqdn_name);
        }
        return result;
    }
};

using NodesAddressesSet = std::unordered_set<NodeAddresses, NodeAddressesHasher>;
using HostPort = std::string;

size_t HashSlot(const std::string& key) {
    size_t start = 0;
    size_t len = 0;
    GetRedisKey(key, &start, &len);
    return std::for_each(key.data() + start, key.data() + start + len, boost::crc_optimal<16, 0x1021>())() & 0x3fff;
}

std::string ParseMovedShard(const std::string& err_string) {
    static const auto kUnknownShard = std::string("");
    size_t pos = err_string.find(' ');  // skip "MOVED" or "ASK"
    if (pos == std::string::npos) return kUnknownShard;
    pos = err_string.find(' ', pos + 1);  // skip hash_slot
    if (pos == std::string::npos) return kUnknownShard;
    pos++;
    size_t end = err_string.find(' ', pos);
    if (end == std::string::npos) end = err_string.size();
    const size_t colon_pos = err_string.rfind(':', end);
    int port = 0;
    try {
        port = std::stoi(err_string.substr(colon_pos + 1, end - (colon_pos + 1)));
    } catch (const std::exception& ex) {
        LOG_WARNING() << "exception in " << __func__ << "(\"" << err_string << "\") " << ex.what();
        return kUnknownShard;
    }
    return err_string.substr(pos, colon_pos - pos) + ":" + std::to_string(port);
}

struct CommandSpecialPrinter {
    const CommandPtr& command;
};

logging::LogHelper& operator<<(logging::LogHelper& os, CommandSpecialPrinter v) {
    const auto& command = v.command;

    if (command->args.GetCommandCount() == 1 || command->invoke_counter + 1 >= command->args.GetCommandCount()) {
        os << command->args;
    } else if (command->invoke_counter < command->args.GetCommandCount()) {
        os << fmt::format(
            "subrequest idx={}, cmd={}", command->invoke_counter, command->args.GetCommandName(command->invoke_counter)
        );
    }

    return os;
}

std::shared_ptr<const std::vector<std::string>> MakeShardNames() {
    /// From suggested max count of nodes ~1000, with replicas, so got ~500 shards
    static const size_t kMaxClusterShards = 500;
    std::vector<std::string> shard_names;
    shard_names.reserve(kMaxClusterShards);
    for (size_t i = 0; i < kMaxClusterShards; ++i) {
        auto number = std::to_string(i);
        if (number.size() < 2) {
            number.insert(0, "0");
        }
        auto name = "shard" + number;
        shard_names.push_back(std::move(name));
    }
    return std::make_shared<const std::vector<std::string>>(std::move(shard_names));
}

void InvokeCommand(CommandPtr command, ReplyPtr&& reply) {
    UASSERT(reply);

    if (reply->server_id.IsAny()) {
        reply->server_id = CommandControlImpl{command->control}.force_server_id;
    }
    LOG_DEBUG() << "redis_request( " << CommandSpecialPrinter{command}
                << " ):" << (reply->status == ReplyStatus::kOk ? '+' : '-') << ":" << reply->time * 1000.0
                << " cc: " << command->control.ToString() << command->GetLogExtra();
    ++command->invoke_counter;
    try {
        command->callback(command, reply);
    } catch (const std::exception& ex) {
        UASSERT(!engine::current_task::IsTaskProcessorThread());
        LOG_WARNING() << "exception in command->callback, cmd=" << reply->cmd << " " << ex << command->GetLogExtra();
    } catch (...) {
        UASSERT(!engine::current_task::IsTaskProcessorThread());
        LOG_WARNING() << "exception in command->callback, cmd=" << reply->cmd << command->GetLogExtra();
    }
}

}  // namespace

class ClusterTopologyHolder final : public TopologyHolderBase {
public:
    using HostPort = std::string;

    ClusterTopologyHolder(
        const engine::ev::ThreadControl& sentinel_thread_control,
        const std::shared_ptr<engine::ev::ThreadPool>& redis_thread_pool,
        std::string shard_group_name,
        Password password,
        const std::vector<std::string>& /*shards*/,
        const std::vector<ConnectionInfo>& conns,
        ConnectionSecurity connection_security
    )
        : ev_thread_(sentinel_thread_control),
          redis_thread_pool_(redis_thread_pool),
          shard_group_name_(std::move(shard_group_name)),
          password_(std::move(password)),
          shards_names_(MakeShardNames()),
          conns_(conns),
          update_topology_timer_(
              ev_thread_,
              [this] { UpdateClusterTopology(); },
              kSentinelGetHostsCheckInterval
          ),
          update_topology_watch_(
              ev_thread_,
              [this] {
                  UpdateClusterTopology();
                  update_topology_watch_.Start();
              }
          ),
          explore_nodes_watch_(
              ev_thread_,
              [this] {
                  ExploreNodes();
                  explore_nodes_watch_.Start();
              }
          ),
          explore_nodes_timer_(
              ev_thread_,
              [this] { ExploreNodes(); },
              kSentinelGetHostsCheckInterval
          ),
          create_nodes_watch_(
              ev_thread_,
              [this] {
                  CreateNodes();
                  create_nodes_watch_.Start();
              }
          ),
          delete_expired_nodes_timer_(
              ev_thread_,
              [this] { DeleteNodes(); },
              kDeleteNodesCheckInterval
          ),
          sentinels_process_creation_timer_(
              ev_thread_,
              [this] {
                  if (sentinels_) {
                      sentinels_->ProcessCreation(redis_thread_pool_);
                      sentinels_->ProcessStateUpdate();
                  }
              },
              kProcessCreationInterval
          ),
          sentinels_process_creation_watch_(
              ev_thread_,
              [this] {
                  if (sentinels_) {
                      sentinels_->ProcessCreation(redis_thread_pool_);
                  }
                  sentinels_process_creation_watch_.Start();
              }
          ),
          sentinels_process_state_update_watch_(
              ev_thread_,
              [this] {
                  if (sentinels_) {
                      sentinels_->ProcessStateUpdate();
                  }
                  sentinels_process_state_update_watch_.Start();
              }
          ),
          is_topology_received_(false),
          update_cluster_slots_flag_(false),
          connection_security_(connection_security) {
        LOG_DEBUG() << "Created ClusterTopologyHolder, shard_group_name=" << shard_group_name_;
    }

    ~ClusterTopologyHolder() override = default;

    void Init() override {
        const constexpr bool kClusterMode = true;
        Shard::Options shard_options;
        shard_options.shard_name = "(sentinel)";
        shard_options.shard_group_name = shard_group_name_;
        shard_options.cluster_mode = kClusterMode;
        shard_options.connection_infos = conns_;
        shard_options.ready_change_callback = [this](bool ready) {
            if (ready) {
                sentinels_process_creation_watch_.Send();
                SendUpdateClusterTopology();
            }
        };

        sentinels_ = std::make_shared<Shard>(std::move(shard_options));

        sentinels_->SignalInstanceStateChange().connect([this](ServerId id, Redis::State state) {
            LOG_TRACE() << "Signaled server " << id.GetDescription() << " state=" << StateToString(state);
            if (state != Redis::State::kInit) sentinels_process_state_update_watch_.Send();
        });
        sentinels_->SignalInstanceReady().connect([this](ServerId, bool /*readonly*/) {
            if (!first_entry_point_connected_.exchange(true)) explore_nodes_watch_.Send();
        });
        sentinels_->ProcessCreation(redis_thread_pool_);
    }

    void Start() override {
        update_topology_watch_.Start();
        update_topology_timer_.Start();
        create_nodes_watch_.Start();
        explore_nodes_watch_.Start();
        explore_nodes_timer_.Start();
        delete_expired_nodes_timer_.Start();
        sentinels_process_creation_watch_.Start();
        sentinels_process_state_update_watch_.Start();
        sentinels_process_creation_timer_.Start();
    }

    void Stop() override {
        signal_node_state_change_.disconnect_all_slots();
        signal_topology_changed_.disconnect_all_slots();

        ev_thread_.RunInEvLoopBlocking([this] {
            update_topology_watch_.Stop();
            create_nodes_watch_.Stop();
            explore_nodes_watch_.Stop();

            update_topology_timer_.Stop();
            explore_nodes_timer_.Stop();
            delete_expired_nodes_timer_.Stop();
            sentinels_process_creation_timer_.Stop();
        });

        sentinels_->Clean();
        topology_.Cleanup();
        nodes_.Clear();
    }

    bool WaitReadyOnce(engine::Deadline deadline, WaitConnectedMode mode) override {
        std::unique_lock lock{mutex_};
        return cv_.WaitUntil(lock, deadline, [this, mode]() {
            if (!IsInitialized()) return false;
            auto ptr = topology_.Read();
            return ptr->IsReady(mode);
        });
    }

    rcu::ReadablePtr<ClusterTopology, rcu::BlockingRcuTraits> GetTopology() const override { return topology_.Read(); }

    void SendUpdateClusterTopology() override { update_topology_watch_.Send(); }

    std::shared_ptr<Redis> GetRedisInstance(const HostPort& host_port) const override {
        const auto connection = nodes_.Get(host_port);
        if (connection) {
            return std::const_pointer_cast<Redis>(connection->Get());
        }
        if (auto ip = ip_by_fqdn_.Get(host_port); ip) {
            const auto connection = nodes_.Get(*ip);
            if (connection) {
                return std::const_pointer_cast<Redis>(connection->Get());
            }
        }

        return {};
    }

    void GetStatistics(SentinelStatistics& stats, const MetricsSettings& settings) const override;

    void SetCommandsBufferingSettings(CommandsBufferingSettings settings) override {
        {
            auto settings_ptr = commands_buffering_settings_.Lock();
            if (*settings_ptr == settings) {
                return;
            }
            *settings_ptr = settings;
        }
        for (const auto& node : nodes_) {
            node.second->SetCommandsBufferingSettings(settings);
        }
    }

    void SetReplicationMonitoringSettings(ReplicationMonitoringSettings settings) override {
        {
            auto settings_ptr = monitoring_settings_.Lock();
            *settings_ptr = settings;
        }
        for (const auto& node : nodes_) {
            node.second->SetReplicationMonitoringSettings(settings);
        }
    }

    void SetRetryBudgetSettings(const utils::RetryBudgetSettings& settings) override {
        {
            auto settings_ptr = retry_budget_settings_.Lock();
            *settings_ptr = settings;
        }
        for (const auto& node : nodes_) {
            node.second->SetRetryBudgetSettings(settings);
        }
    }

    void SetConnectionInfo(const std::vector<ConnectionInfoInt>& info_array) override {
        sentinels_->SetConnectionInfo(info_array);
    }

    static size_t GetClusterSlotsCalledCounter() { return cluster_slots_call_counter_.load(std::memory_order_relaxed); }

    boost::signals2::signal<void(HostPort, Redis::State)>& GetSignalNodeStateChanged() override {
        return signal_node_state_change_;
    }

    boost::signals2::signal<void(size_t)>& GetSignalTopologyChanged() override { return signal_topology_changed_; }

    void UpdatePassword(const Password& password) override {
        auto lock = password_.UniqueLock();
        *lock = password;
    }

    Password GetPassword() override {
        const auto lock = password_.Lock();
        return *lock;
    }

    std::string GetReadinessInfo() const override {
        return fmt::format(
            "Nodes received: {}; topology received: {}.", is_nodes_received_.load(), is_topology_received_.load()
        );
    }

private:
    std::shared_ptr<RedisConnectionHolder> CreateRedisInstance(const HostPort& host_port);

    engine::ev::ThreadControl ev_thread_;
    std::shared_ptr<engine::ev::ThreadPool> redis_thread_pool_;

    const std::string shard_group_name_;
    concurrent::Variable<Password, std::mutex> password_;
    std::shared_ptr<const std::vector<std::string>> shards_names_;
    const std::vector<ConnectionInfo> conns_;
    std::shared_ptr<Shard> sentinels_;

    std::atomic_size_t current_topology_version_{0};
    rcu::Variable<ClusterTopology, rcu::BlockingRcuTraits> topology_;

    /// Update cluster topology
    /// @{
    engine::ev::PeriodicWatcher update_topology_timer_;
    engine::ev::AsyncWatcher update_topology_watch_;
    void UpdateClusterTopology();
    /// @}

    /// Discover actual nodes in cluster
    engine::ev::AsyncWatcher explore_nodes_watch_;
    engine::ev::PeriodicWatcher explore_nodes_timer_;
    std::atomic<bool> first_entry_point_connected_{false};
    void ExploreNodes();

    /// Create connections to discovered nodes
    engine::ev::AsyncWatcher create_nodes_watch_;
    void CreateNodes();

    /// Delete non-actual nodes
    engine::ev::PeriodicWatcher delete_expired_nodes_timer_;
    void DeleteNodes();

    engine::ev::PeriodicWatcher sentinels_process_creation_timer_;
    engine::ev::AsyncWatcher sentinels_process_creation_watch_;
    engine::ev::AsyncWatcher sentinels_process_state_update_watch_;

    ///{ Wait ready
    std::mutex mutex_;
    engine::impl::ConditionVariableAny<std::mutex> cv_;
    std::atomic<bool> is_topology_received_{false};
    std::atomic<bool> is_nodes_received_{false};
    std::atomic<bool> update_cluster_slots_flag_{false};
    bool IsInitialized() const { return is_nodes_received_.load() && is_topology_received_.load(); }
    ///}

    boost::signals2::signal<void(HostPort, Redis::State)> signal_node_state_change_;
    boost::signals2::signal<void(size_t shards_count)> signal_topology_changed_;
    NodesStorage nodes_;

    concurrent::Variable<std::optional<CommandsBufferingSettings>, std::mutex> commands_buffering_settings_;
    concurrent::Variable<ReplicationMonitoringSettings, std::mutex> monitoring_settings_;
    concurrent::Variable<utils::RetryBudgetSettings, std::mutex> retry_budget_settings_;
    concurrent::Variable<std::unordered_set<HostPort>, std::mutex> nodes_to_create_;
    concurrent::Variable<std::unordered_set<HostPort>, std::mutex> actual_nodes_;
    // work only from sentinel thread so no need to synchronize it
    std::unordered_map<HostPort, utils::datetime::SteadyCoarseClock::time_point> nodes_last_seen_time_;
    rcu::RcuMap<std::string, std::string, StdMutexRcuMapTraits<std::string>> ip_by_fqdn_;

    static std::atomic<size_t> cluster_slots_call_counter_;

    const ConnectionSecurity connection_security_;
};

std::atomic<size_t> ClusterTopologyHolder::cluster_slots_call_counter_(0);

namespace {

enum class ClusterNodesResponseStatus {
    kOk,
    kFail,
    kNonCluster,
};

std::optional<std::string> GetHostNameFromClusterNodesLine(std::string_view line, std::string_view port) {
    auto it = line.rfind(',');
    if (it == std::string_view::npos) {
        return std::nullopt;
    }
    return std::string(line.substr(it + 1)) + ":" + std::string(port);
}

ClusterNodesResponseStatus ParseClusterNodesResponse(const ReplyPtr& reply, NodesAddressesSet& res) {
    UASSERT(reply);
    if (reply->IsUnknownCommandError()) {
        return ClusterNodesResponseStatus::kNonCluster;
    }

    if (!reply->IsOk()) {
        return ClusterNodesResponseStatus::kFail;
    }

    if (!reply->data.IsString()) {
        return ClusterNodesResponseStatus::kFail;
    }
    const auto& host_lines = utils::text::SplitIntoStringViewVector(reply->data.GetString(), "\n");

    for (const auto& host_line : host_lines) {
        const auto& splitted = utils::text::SplitIntoStringViewVector(host_line, " ");
        if (splitted.size() < 2) {
            continue;
        }

        const auto& host_port_communication_port = splitted[1];
        if (host_port_communication_port == ":0@0" || host_port_communication_port == ":0") {
            continue;
        }
        const auto host_port_it = host_port_communication_port.rfind('@');
        auto host_port = host_port_communication_port.substr(0, host_port_it);

        const auto port_it = host_port.rfind(':');
        if (port_it == std::string::npos) {
            return ClusterNodesResponseStatus::kFail;
        }
        auto port = host_port.substr(port_it + 1);
        NodeAddresses addrs;
        addrs.ip = std::move(host_port);
        addrs.fqdn_name = GetHostNameFromClusterNodesLine(host_port_communication_port, port);
        res.emplace(addrs);
    }

    return ClusterNodesResponseStatus::kOk;
}

}  // namespace

void ClusterTopologyHolder::ExploreNodes() {
    /// Call cluster nodes, parse, prepare list of new hosts to create
    if (!sentinels_) {
        return;
    }

    const auto cmd = PrepareCommand({"CLUSTER", "NODES"}, [this](const CommandPtr& /*cmd*/, ReplyPtr reply) {
        NodesAddressesSet host_ports;
        std::unordered_set<HostPort> host_ports_to_create;

        if (ParseClusterNodesResponse(reply, host_ports) != ClusterNodesResponseStatus::kOk) {
            LOG_WARNING() << "Failed to parse CLUSTER NODES response";
            return;
        }

        for (const auto& host_port : host_ports) {
            if (!nodes_.Get(host_port.ip)) {
                host_ports_to_create.insert(host_port.ip);
            }
        }
        if (!host_ports.empty()) {
            for (const auto& [ip, fqdn] : host_ports) {
                if (!fqdn.has_value()) {
                    continue;
                }
                auto ptr = ip_by_fqdn_.Get(*fqdn);
                if (!ptr || *ptr != ip) {
                    ip_by_fqdn_.InsertOrAssign(*fqdn, std::make_shared<std::string>(ip));
                }
            }

            std::unordered_set<HostPort> ips;
            {
                for (auto& addr : host_ports) {
                    ips.insert(std::move(addr.ip));
                }
                auto ptr = actual_nodes_.Lock();
                ptr->merge(std::move(ips));
            }
        }

        if (!host_ports_to_create.empty()) {
            {
                auto ptr = nodes_to_create_.Lock();
                std::swap(*ptr, host_ports_to_create);
            }
            create_nodes_watch_.Send();
        }
    });
    sentinels_->AsyncCommand(cmd);
}

void ClusterTopologyHolder::CreateNodes() {
    std::unordered_set<HostPort> host_ports_to_create;
    {
        auto ptr = nodes_to_create_.Lock();
        std::swap(*ptr, host_ports_to_create);
    }

    for (auto&& host_port : host_ports_to_create) {
        auto instance = CreateRedisInstance(host_port);
        instance->signal_state_change.connect([host_port, this](redis::RedisState state) {
            GetSignalNodeStateChanged()(host_port, state);
            { const std::lock_guard lock{mutex_}; }  // do not lose the notify
            cv_.NotifyAll();
        });
        nodes_.Insert(std::move(host_port), std::move(instance));
    }

    if (!is_nodes_received_.exchange(true)) {
        SendUpdateClusterTopology();
    }
}

void ClusterTopologyHolder::DeleteNodes() {
    std::unordered_set<HostPort> actual_nodes;
    {
        auto ptr = actual_nodes_.Lock();
        std::swap(*ptr, actual_nodes);
    }
    const auto now = utils::datetime::SteadyCoarseClock::now();
    for (auto& node : actual_nodes) {
        nodes_last_seen_time_[node] = now;
    }
    utils::EraseIf(nodes_last_seen_time_, [this, now](const auto& node_time) {
        const auto& [node, time] = node_time;
        if (now - time >= kDeleteNodeInterval) {
            nodes_.Erase(node);
            return true;
        }
        return false;
    });
}

void ClusterSentinelImpl::ProcessWaitingCommands() {
    std::vector<SentinelCommand> waiting_commands;

    {
        const std::lock_guard<std::mutex> lock(command_mutex_);
        waiting_commands.swap(commands_);
    }
    if (!waiting_commands.empty()) {
        LOG_INFO() << "ProcessWaitingCommands client=" << client_name_ << " shard_group_name=" << shard_group_name_
                   << " waiting_commands.size()=" << waiting_commands.size();
    }

    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    for (const SentinelCommand& scommand : waiting_commands) {
        const auto& command = scommand.command;
        const CommandControlImpl cc{command->control};
        if (scommand.start + cc.timeout_all < now) {
            for (const auto& args : command->args) {
                auto reply = std::make_shared<Reply>(
                    args.GetCommandName(),
                    ReplyData::CreateError("Command in the send queue timed out"),
                    ReplyStatus::kTimeoutError
                );
                statistics_internal_.redis_not_ready++;
                InvokeCommand(command, std::move(reply));
            }
        } else {
            AsyncCommand(scommand, kDefaultPrevInstanceIdx);
        }
    }
}

void ClusterSentinelImpl::ProcessWaitingCommandsOnStop() {
    std::vector<SentinelCommand> waiting_commands;

    {
        const std::lock_guard<std::mutex> lock(command_mutex_);
        waiting_commands.swap(commands_);
    }

    for (const SentinelCommand& scommand : waiting_commands) {
        const auto& command = scommand.command;
        for (const auto& args : command->args) {
            auto reply = std::make_shared<Reply>(
                args.GetCommandName(),
                ReplyData::CreateError("Stopping, killing commands remaining in send queue"),
                ReplyStatus::kTimeoutError
            );
            statistics_internal_.redis_not_ready++;
            InvokeCommand(command, std::move(reply));
        }
    }
}

std::shared_ptr<RedisConnectionHolder> ClusterTopologyHolder::CreateRedisInstance(const std::string& host_port) {
    const auto port_it = host_port.rfind(':');
    UINVARIANT(port_it != std::string::npos, "port must be delimited by ':'");
    const auto port_str = host_port.substr(port_it + 1);
    const auto port = std::stoi(port_str);
    const auto host = host_port.substr(0, port_it);
    const auto buffering_settings_ptr = commands_buffering_settings_.Lock();
    const auto replication_monitoring_settings_ptr = monitoring_settings_.Lock();
    const auto retry_budget_settings_ptr = retry_budget_settings_.Lock();
    LOG_DEBUG() << "Create new redis instance " << host_port;
    auto creation_settings = RedisConnectionHolder::makeDefaultRedisCreationSettings();
    creation_settings.connection_security = connection_security_;
    return RedisConnectionHolder::Create(
        ev_thread_,
        redis_thread_pool_,
        host,
        port,
        GetPassword(),
        kClusterDatabaseIndex,
        buffering_settings_ptr->value_or(CommandsBufferingSettings{}),
        *replication_monitoring_settings_ptr,
        *retry_budget_settings_ptr,
        creation_settings
    );
}

namespace {

template <typename Callback>
std::shared_ptr<utils::FastScopeGuard<Callback>> MakeSharedScopeGuard(Callback cb) {
    return std::make_shared<utils::FastScopeGuard<Callback>>(std::move(cb));
}

}  // namespace

void ClusterTopologyHolder::UpdateClusterTopology() {
    if (!is_nodes_received_) {
        LOG_DEBUG() << "Skip updating cluster topology: no nodes yet";
        return;
    };
    if (update_cluster_slots_flag_.exchange(true)) return;
    auto reset_update_cluster_slots_ = MakeSharedScopeGuard([&]() noexcept { update_cluster_slots_flag_ = false; });
    /// Update sentinel
    sentinels_->ProcessCreation(redis_thread_pool_);

    /// Update controlled topology. Go to CLUSTER SLOTS
    /// ...
    ProcessGetClusterHostsRequest(
        shards_names_,
        GetClusterHostsRequest(*sentinels_, GetPassword()),
        [this, reset{std::move(reset_update_cluster_slots_)}](
            ClusterShardHostInfos shard_infos, size_t requests_sent, size_t responses_parsed, bool is_non_cluster_error
        ) {
            LOG_DEBUG() << "Parsing response from cluster slots: shard_infos.size(): " << shard_infos.size()
                        << ", requests_sent=" << requests_sent << ", responses_parsed=" << responses_parsed;
            const auto deferred = utils::FastScopeGuard([&]() noexcept { ++cluster_slots_call_counter_; });
            if (is_non_cluster_error) {
                LOG_DEBUG() << "Non cluster error: shard_infos.size(): " << shard_infos.size();
                throw std::runtime_error("Redis must be in cluster mode");
            }
            if (shard_infos.empty()) {
                LOG_WARNING() << "Received empty topology";
                return;
            }

            if (!CheckQuorum(requests_sent, responses_parsed)) {
                LOG_WARNING() << "Too many 'cluster slots' requests failed: requests_sent=" << requests_sent
                              << " responses_parsed=" << responses_parsed;
                return;
            }

            {
                auto temp_read_ptr = topology_.Read();
                if (temp_read_ptr->HasSameInfos(shard_infos)) {
                    /// Nothing new here so do nothing
                    return;
                }
            }

            auto topology = ClusterTopology(
                ++current_topology_version_,
                std::chrono::steady_clock::now(),
                std::move(shard_infos),
                redis_thread_pool_,
                nodes_
            );
            /// Run in ev_thread because topology_.Assign can free some old
            /// topologies with their related redis connections, and these
            /// connections must be freed on "sentinel" thread.
            ev_thread_.RunInEvLoopAsync([this, topology{std::move(topology)}]() mutable {
                try {
                    const auto new_shards_count = topology.GetShardsCount();
                    topology_.Assign(std::move(topology));
                    signal_topology_changed_(new_shards_count);
                } catch (const rcu::MissingKeyException& e) {
                    LOG_WARNING() << "Failed to update cluster topology: " << e;
                    return;
                }
                is_topology_received_ = true;
                { const std::lock_guard lock{mutex_}; }  // do not lose the notify
                cv_.NotifyAll();

                LOG_DEBUG() << "Cluster topology updated to version" << current_topology_version_.load();
            });
        }
    );
}

void ClusterTopologyHolder::GetStatistics(SentinelStatistics& stats, const MetricsSettings& settings) const {
    if (sentinels_) {
        stats.sentinel.emplace(ShardStatistics(settings));
        sentinels_->GetStatistics(true, settings, *stats.sentinel);
    }
    stats.internal.is_autotoplogy = true;
    stats.internal.cluster_topology_checks =
        utils::statistics::Rate{cluster_slots_call_counter_.load(std::memory_order_relaxed)};
    stats.internal.cluster_topology_updates =
        utils::statistics::Rate{current_topology_version_.load(std::memory_order_relaxed)};

    auto topology = GetTopology();
    topology->GetStatistics(settings, stats);
}

ClusterSentinelImpl::ClusterSentinelImpl(
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
)
    : sentinel_obj_(sentinel),
      ev_thread_(sentinel_thread_control),
      process_waiting_commands_timer_(std::make_unique<engine::ev::PeriodicWatcher>(
          ev_thread_,
          [this] { ProcessWaitingCommands(); },
          kSentinelGetHostsCheckInterval
      )),
      shard_group_name_(std::move(shard_group_name)),
      conns_(conns),
      redis_thread_pool_(redis_thread_pool),
      client_name_(client_name),
      dynamic_config_source_(std::move(dynamic_config_source)) {
    if (!key_shard) {
        topology_holder_ = std::make_unique<ClusterTopologyHolder>(
            ev_thread_, redis_thread_pool, shard_group_name, password, shards, conns, connection_security
        );
    } else {
        LOG_DEBUG() << "Construct Standalone topology holder";
        UASSERT_MSG(conns.size() == 1, "In standalone mode we expect exactly one redis node to connect!");
        // TODO: TAXICOMMON-10376 experiment with providing kClusterDatabaseIndex other than 0 for standalone mode
        topology_holder_ = std::make_unique<StandaloneTopologyHolder>(
            ev_thread_, redis_thread_pool, password, kClusterDatabaseIndex, conns.front()
        );
    }
}

ClusterSentinelImpl::~ClusterSentinelImpl() { Stop(); }

std::unordered_map<ServerId, size_t, ServerIdHasher> ClusterSentinelImpl::GetAvailableServersWeighted(
    size_t /*shard_idx*/,
    bool with_master,
    const CommandControl& cc
) const {
    auto topology = topology_holder_->GetTopology();
    /// Method used only in Subscribe. When using cluster mode every node
    /// can listen messages from any node. So ignore concrete shard and use
    /// kUnknown to get all of cluster nodes.
    return topology->GetAvailableServersWeighted(kUnknownShard, with_master, cc);
}

void ClusterSentinelImpl::WaitConnectedDebug(bool /*allow_empty_slaves*/) {
    const RedisWaitConnected wait_connected{
        WaitConnectedMode::kMasterAndSlave, true, kRedisWaitConnectedDefaultTimeout};
    WaitConnectedOnce(wait_connected);
}

void ClusterSentinelImpl::WaitConnectedOnce(RedisWaitConnected wait_connected) {
    auto deadline = engine::Deadline::FromDuration(wait_connected.timeout);
    if (!topology_holder_->WaitReadyOnce(deadline, wait_connected.mode)) {
        auto topology = topology_holder_->GetTopology();
        const std::string msg = fmt::format(
            "Failed to init cluster slots for redis, shard_group_name={} in {} "
            "ms, mode={}. {} {}",
            shard_group_name_,
            wait_connected.timeout.count(),
            ToString(wait_connected.mode),
            topology_holder_->GetReadinessInfo(),
            topology->GetReadinessInfo()
        );
        if (wait_connected.throw_on_fail)
            throw ClientNotConnectedException(msg);
        else {
            LOG_WARNING() << msg << ", starting with not ready Redis client";
        }
    }
}

void ClusterSentinelImpl::ForceUpdateHosts() { throw std::runtime_error(std::string(__func__) + " Unimplemented yet"); }

void ClusterSentinelImpl::Init() {
    topology_holder_->GetSignalNodeStateChanged().connect([this](std::string host_port, Redis::State /*state*/) {
        const auto topology = topology_holder_->GetTopology();
        const auto shard_opt = topology->GetShardByHostPort(host_port);
        if (!shard_opt) {
            // changed state of node not used in cluster (e.g. no slots assigned)
            return;
        }

        sentinel_obj_.NotifyInstancesChanged(*shard_opt);
    });

    topology_holder_->GetSignalTopologyChanged().connect([this](size_t shards_count) {
        sentinel_obj_.NotifyTopologyChanged(shards_count);
    });

    topology_holder_->Init();
}

void ClusterSentinelImpl::AsyncCommand(const SentinelCommand& scommand, size_t prev_instance_idx) {
    if (!AdjustDeadline(scommand, dynamic_config_source_.GetSnapshot())) {
        auto reply =
            std::make_shared<Reply>("", ReplyData::CreateError("Deadline propagation"), ReplyStatus::kTimeoutError);
        InvokeCommand(scommand.command, std::move(reply));
        return;
    }

    const CommandPtr command = scommand.command;
    const size_t shard = scommand.shard;
    const bool master = scommand.master;
    const auto start = scommand.start;
    const auto counter = command->counter;
    const CommandPtr command_check_errors(PrepareCommand(
        std::move(command->args),
        [this, shard, master, start, counter, command](const CommandPtr& ccommand, ReplyPtr reply) {
            if (counter != command->counter) return;
            UASSERT(reply);

            const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

            const bool error_ask = reply->data.IsErrorAsk();
            const bool error_moved =
                reply->data.IsErrorMoved()
                // *SUBSCRIBE commands have logic in FSM, those commands do not expect retries on move.
                // The behavior is tested in redis/functional_tests/cluster_auto_topology_pubsub/tests
                && ccommand->args.GetCommandCount() == 1 && !ccommand->args.begin()->IsSubscribeCommand();

            if (error_moved) {
                LOG_DEBUG() << "MOVED c.instance_idx:" << ccommand->instance_idx << " shard: " << shard
                            << " movedto:" << ParseMovedShard(reply->data.GetError()) << " args:" << ccommand->args;
                this->topology_holder_->SendUpdateClusterTopology();
            }
            const bool retry_to_master =
                !master && reply->data.IsNil() && command->control.force_retries_to_master_on_nil_reply;
            const bool retry = retry_to_master || reply->status != ReplyStatus::kOk || error_ask || error_moved ||
                               reply->IsUnusableInstanceError() || reply->IsReadonlyError() ||
                               reply->data.IsErrorClusterdown();

            std::shared_ptr<Redis> moved_to_instance;
            if (retry) {
                const CommandControlImpl cc{command->control};
                const size_t new_shard = shard;
                size_t retries_left = cc.max_retries - 1;
                if (error_ask || error_moved) {
                    LOG_DEBUG() << "Got error '" << reply->data.GetError() << "' reply, cmd=" << reply->cmd
                                << ", server=" << reply->server_id.GetDescription();
                    const auto& host_port = ParseMovedShard(reply->data.GetError());
                    command->counter++;
                    if (!command->redirected || (error_ask && !command->asking)) ++retries_left;
                    moved_to_instance = topology_holder_->GetRedisInstance(host_port);
                    if (!moved_to_instance) {
                        LOG_WARNING() << "moved to unknown host " << host_port;
                        /// Can we do something else?? We don't have client for this
                        /// redis instance
                        return;
                    }
                }
                const std::chrono::steady_clock::time_point until = start + cc.timeout_all;
                if (now < until && retries_left > 0) {
                    const auto timeout_all = std::chrono::duration_cast<std::chrono::milliseconds>(until - now);
                    command->control.timeout_single = std::min(cc.timeout_single, timeout_all);
                    command->control.timeout_all = timeout_all;
                    command->control.max_retries = retries_left;

                    auto new_command = PrepareCommand(
                        ccommand->args.Clone(),
                        [command](const CommandPtr& cmd, ReplyPtr reply) {
                            if (command->callback) command->callback(cmd, std::move(reply));
                        },
                        command->control,
                        command->counter + 1,
                        command->asking || error_ask,
                        0,
                        error_ask || error_moved
                    );
                    new_command->log_extra = std::move(command->log_extra);
                    if (moved_to_instance) {
                        moved_to_instance->AsyncCommand(new_command);
                    } else {
                        AsyncCommand(
                            SentinelCommand(
                                new_command,
                                master || retry_to_master || (error_moved && shard == new_shard),
                                new_shard,
                                start
                            ),
                            ccommand->instance_idx
                        );
                    }
                    return;
                }
            }

            const std::chrono::duration<double> time = now - start;
            reply->time = time.count();
            command->args = std::move(ccommand->args);
            InvokeCommand(command, std::move(reply));
            ccommand->args = std::move(command->args);
        },
        command->control,
        command->counter,
        command->asking,
        prev_instance_idx,
        false,
        !master
    ));

    const auto topology = topology_holder_->GetTopology();
    const auto& master_shard = topology->GetClusterShardByIndex(shard);
    if (!master_shard.AsyncCommand(command_check_errors)) {
        scommand.command->args = std::move(command_check_errors->args);
        AsyncCommandFailed(scommand);
        return;
    }
}

size_t ClusterSentinelImpl::ShardByKey(const std::string& key) const {
    const auto slot = HashSlot(key);
    const auto ptr = topology_holder_->GetTopology();
    return ptr->GetShardIndexBySlot(slot);
}

void ClusterSentinelImpl::Start() {
    Init();
    LOG_DEBUG() << "Created ClusterSentinelImpl, shard_group_name=" << shard_group_name_;

    topology_holder_->Start();
    process_waiting_commands_timer_->Start();
}

void ClusterSentinelImpl::AsyncCommandFailed(const SentinelCommand& scommand) {
    // Run command callbacks from redis thread only.
    // It prevents recursive mutex locking in subscription_storage.
    EnqueueCommand(scommand);
}

void ClusterSentinelImpl::Stop() {
    UASSERT(engine::current_task::IsTaskProcessorThread());
    topology_holder_->Stop();
    ev_thread_.RunInEvLoopBlocking([this] {
        process_waiting_commands_timer_->Stop();
        ProcessWaitingCommandsOnStop();
    });
}

void ClusterSentinelImpl::SetCommandsBufferingSettings(CommandsBufferingSettings commands_buffering_settings) {
    if (topology_holder_) {
        topology_holder_->SetCommandsBufferingSettings(commands_buffering_settings);
    }
}

void ClusterSentinelImpl::SetReplicationMonitoringSettings(const ReplicationMonitoringSettings& monitoring_settings) {
    if (topology_holder_) {
        topology_holder_->SetReplicationMonitoringSettings(monitoring_settings);
    }
}

void ClusterSentinelImpl::SetRetryBudgetSettings(const utils::RetryBudgetSettings& settings) {
    if (topology_holder_) {
        topology_holder_->SetRetryBudgetSettings(settings);
    }
}

SentinelStatistics ClusterSentinelImpl::GetStatistics(const MetricsSettings& settings) const {
    if (!topology_holder_) {
        return {settings, {}};
    }

    SentinelStatistics stats(settings, statistics_internal_);
    topology_holder_->GetStatistics(stats, settings);
    return stats;
}

void ClusterSentinelImpl::EnqueueCommand(const SentinelCommand& command) {
    const std::lock_guard<std::mutex> lock(command_mutex_);
    commands_.push_back(command);
}

size_t ClusterSentinelImpl::ShardsCount() const {
    const auto ptr = topology_holder_->GetTopology();
    const auto res = ptr->GetShardsCount();
    UASSERT(res != 0);
    return res;
}

size_t ClusterSentinelImpl::GetClusterSlotsCalledCounter() {
    return ClusterTopologyHolder::GetClusterSlotsCalledCounter();
}

void ClusterSentinelImpl::SetConnectionInfo(const std::vector<ConnectionInfoInt>& info_array) {
    topology_holder_->SetConnectionInfo(info_array);
}

void ClusterSentinelImpl::UpdatePassword(const Password& password) { topology_holder_->UpdatePassword(password); }

PublishSettings ClusterSentinelImpl::GetPublishSettings() {
    return PublishSettings{kUnknownShard, false, CommandControl::Strategy::kEveryDc};
}

}  // namespace storages::redis::impl

// NOLINTEND(clang-analyzer-cplusplus.NewDelete)

USERVER_NAMESPACE_END
