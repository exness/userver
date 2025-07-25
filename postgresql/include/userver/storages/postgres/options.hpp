#pragma once

/// @file userver/storages/postgres/options.hpp
/// @brief Options

#include <chrono>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <unordered_map>

#include <userver/congestion_control/controllers/linear.hpp>
#include <userver/storages/postgres/postgres_fwd.hpp>
#include <userver/utils/impl/transparent_hash.hpp>
#include <userver/utils/str_icase.hpp>
#include <userver/utils/string_literal.hpp>

USERVER_NAMESPACE_BEGIN

namespace storages::postgres {

/*! [Isolation levels] */
/// @brief SQL transaction isolation level
/// @see https://www.postgresql.org/docs/current/static/sql-set-transaction.html
enum class IsolationLevel : std::uint16_t {
    kReadCommitted,   //!< READ COMMITTED
    kRepeatableRead,  //!< REPEATABLE READ
    kSerializable,    //!< SERIALIZABLE
    kReadUncommitted  //!< READ UNCOMMITTED @warning In Postgres READ UNCOMMITTED
                      //!< is treated as READ COMMITTED
};
/*! [Isolation levels] */

std::ostream& operator<<(std::ostream&, IsolationLevel);

/// @brief PostgreSQL transaction options
///
/// A transaction can be started using all isolation levels and modes
/// supported by PostgreSQL server as specified in it's documentation.
///
/// Default isolation level is READ COMMITTED, default mode is READ WRITE.
/// @code
/// // Read-write read committed transaction.
/// TransactionOptions opts;
/// @endcode
///
/// Transaction class provides constants Transaction::RW, Transaction::RO and
/// Transaction::Deferrable for convenience.
///
/// Other variants can be created with TransactionOptions constructors
/// that are constexpr.
///
/// @see https://www.postgresql.org/docs/current/static/sql-set-transaction.html
struct TransactionOptions {
    /*! [Transaction modes] */
    enum Mode : std::uint16_t {
        kReadWrite = 0,
        kReadOnly = 1,
        kDeferrable = 3  //!< Deferrable transaction is read only
    };
    /*! [Transaction modes] */
    IsolationLevel isolation_level = IsolationLevel::kReadCommitted;
    Mode mode = kReadWrite;

    constexpr TransactionOptions() = default;
    constexpr explicit TransactionOptions(IsolationLevel lvl) : isolation_level{lvl} {}
    constexpr TransactionOptions(IsolationLevel lvl, Mode m) : isolation_level{lvl}, mode{m} {}
    constexpr explicit TransactionOptions(Mode m) : mode{m} {}

    bool IsReadOnly() const { return mode & kReadOnly; }

    /// The deferrable property has effect only if the transaction is also
    /// serializable and read only
    static constexpr TransactionOptions Deferrable() { return {IsolationLevel::kSerializable, kDeferrable}; }
};

constexpr inline bool operator==(TransactionOptions lhs, TransactionOptions rhs) {
    return lhs.isolation_level == rhs.isolation_level && lhs.mode == rhs.mode;
}
USERVER_NAMESPACE::utils::StringLiteral BeginStatement(TransactionOptions opts) noexcept;

/// A structure to control timeouts for PosrgreSQL queries
///
/// There are two parameters, `execute` and `statement`.
///
/// `execute` parameter controls the overall time the driver spends executing a
/// query, that includes:
/// * connecting to PostgreSQL server, if there are no connections available and
///   connection pool still has space for new connections;
/// * waiting for a connection to become idle if there are no idle connections
///   and connection pool already has reached it's max size;
/// * preparing a statement if the statement is run for the first time on the
///   connection;
/// * binding parameters and executing the statement;
/// * waiting for the first results to arrive from the server. If the result set
///   is big, only time to the first data packet is taken into account.
///
/// `statement` is rather straightforward, it's the PostgreSQL server-side
/// parameter, and it controls the time the database backend can spend executing
/// a single statement. It is very costly to change the statement timeout
/// often, as it requires a roundtrip to the database to change the setting.
/// @see https://www.postgresql.org/docs/12/runtime-config-client.html
///
/// `execute` timeout should always be greater than the `statement` timeout!
///
/// In case of a timeout, either back-end or overall, the client gets an
/// exception and the driver tries to clean up the connection for further reuse.
struct CommandControl {
    /// Overall timeout for a command being executed
    TimeoutDuration network_timeout_ms{};
    /// PostgreSQL server-side timeout
    TimeoutDuration statement_timeout_ms{};

    constexpr CommandControl(TimeoutDuration network_timeout_ms, TimeoutDuration statement_timeout_ms)
        : network_timeout_ms(network_timeout_ms), statement_timeout_ms(statement_timeout_ms) {}

    constexpr CommandControl WithExecuteTimeout(TimeoutDuration n) const noexcept { return {n, statement_timeout_ms}; }

    constexpr CommandControl WithStatementTimeout(TimeoutDuration s) const noexcept { return {network_timeout_ms, s}; }

    bool operator==(const CommandControl& rhs) const {
        return network_timeout_ms == rhs.network_timeout_ms && statement_timeout_ms == rhs.statement_timeout_ms;
    }

    bool operator!=(const CommandControl& rhs) const { return !(*this == rhs); }
};

/// @brief storages::postgres::CommandControl that may not be set
using OptionalCommandControl = std::optional<CommandControl>;

using CommandControlByMethodMap = USERVER_NAMESPACE::utils::impl::TransparentMap<std::string, CommandControl>;
using CommandControlByHandlerMap =
    USERVER_NAMESPACE::utils::impl::TransparentMap<std::string, CommandControlByMethodMap>;
using CommandControlByQueryMap = USERVER_NAMESPACE::utils::impl::TransparentMap<std::string, CommandControl>;

OptionalCommandControl
GetHandlerOptionalCommandControl(const CommandControlByHandlerMap& map, std::string_view path, std::string_view method);

OptionalCommandControl GetQueryOptionalCommandControl(const CommandControlByQueryMap& map, std::string_view query_name);

/// Default initial pool connection count
inline constexpr std::size_t kDefaultPoolMinSize = 4;

/// Default maximum replication lag
inline constexpr auto kDefaultMaxReplicationLag = std::chrono::seconds{60};

/// Default pool connections limit
inline constexpr std::size_t kDefaultPoolMaxSize = 15;

/// Default size of queue for clients waiting for connections
inline constexpr std::size_t kDefaultPoolMaxQueueSize = 200;

/// Default limit for concurrent establishing connections number
inline constexpr std::size_t kDefaultConnectingLimit = 0;

/// @brief PostgreSQL topology options
///
/// Dynamic option @ref POSTGRES_TOPOLOGY_SETTINGS
struct TopologySettings {
    /// Maximum replication lag. Once the replica lag exceeds this value it will be automatically disabled.
    std::chrono::milliseconds max_replication_lag{kDefaultMaxReplicationLag};

    /// List of manually disabled replicas (FQDNs).
    std::unordered_set<std::string, USERVER_NAMESPACE::utils::StrIcaseHash, USERVER_NAMESPACE::utils::StrIcaseEqual>
        disabled_replicas{};
};

/// @brief PostgreSQL connection pool options
///
/// Dynamic option @ref POSTGRES_CONNECTION_POOL_SETTINGS
struct PoolSettings final {
    /// Number of connections created initially
    std::size_t min_size{kDefaultPoolMinSize};

    /// Maximum number of created connections
    std::size_t max_size{kDefaultPoolMaxSize};

    /// Maximum number of clients waiting for a connection
    std::size_t max_queue_size{kDefaultPoolMaxQueueSize};

    /// Limits number of concurrent establishing connections (0 - unlimited)
    std::size_t connecting_limit{kDefaultConnectingLimit};

    bool operator==(const PoolSettings& rhs) const {
        return min_size == rhs.min_size && max_size == rhs.max_size && max_queue_size == rhs.max_queue_size &&
               connecting_limit == rhs.connecting_limit;
    }
};

// Configs with a suffix `Dynamic` are need to compatibility with static:
// We must update only fields that were updated in a dynamic config (not a full config!).
struct PoolSettingsDynamic final {
    std::optional<std::size_t> min_size;
    std::optional<std::size_t> max_size;
    std::optional<std::size_t> max_queue_size;
    std::optional<std::size_t> connecting_limit;
};

/// Default size limit for prepared statements cache
inline constexpr std::size_t kDefaultMaxPreparedCacheSize = 200;

/// Pipeline mode configuration
///
/// Dynamic option @ref POSTGRES_CONNECTION_PIPELINE_EXPERIMENT
enum class PipelineMode { kDisabled, kEnabled };

/// Whether to omit excessive D(escribe) message
/// when executing prepared statements
///
/// Dynamic option @ref POSTGRES_OMIT_DESCRIBE_IN_EXECUTE
enum class OmitDescribeInExecuteMode { kDisabled, kEnabled };

/// PostgreSQL connection options
///
/// Dynamic option @ref POSTGRES_CONNECTION_SETTINGS
struct ConnectionSettings {
    enum PreparedStatementOptions {
        kCachePreparedStatements,
        kNoPreparedStatements,
    };
    enum UserTypesOptions {
        kUserTypesEnabled,
        kUserTypesEnforced,
        kPredefinedTypesOnly,
    };
    enum CheckQueryParamsOptions {
        kIgnoreUnused,
        kCheckUnused,
    };
    enum DiscardOnConnectOptions {
        kDiscardNone,
        kDiscardAll,
    };
    enum StatementLogMode {
        kLogSkip,
        kLog,
    };
    using SettingsVersion = std::size_t;

    /// Cache prepared statements or not
    PreparedStatementOptions prepared_statements = kCachePreparedStatements;

    /// Enables the usage of user-defined types
    UserTypesOptions user_types = kUserTypesEnabled;

    /// Checks for not-NULL query params that are not used in query
    CheckQueryParamsOptions ignore_unused_query_params = kCheckUnused;

    /// Limits the size or prepared statements cache
    std::size_t max_prepared_cache_size = kDefaultMaxPreparedCacheSize;

    /// Turns on connection pipeline mode
    PipelineMode pipeline_mode = PipelineMode::kDisabled;

    /// Enables protocol-level optimization when executing prepared statements
    OmitDescribeInExecuteMode omit_describe_mode = OmitDescribeInExecuteMode::kDisabled;

    /// This many connection errors in 15 seconds block new connections opening
    std::size_t recent_errors_threshold = 2;

    /// The maximum lifetime of the connection after which it will be closed
    std::optional<std::chrono::seconds> max_ttl{};

    /// Execute discard all after establishing a new connection
    DiscardOnConnectOptions discard_on_connect = kDiscardAll;

    /// Statement logging in span tags
    StatementLogMode statement_log_mode = kLog;

    bool deadline_propagation_enabled = true;

    /// Helps keep track of the changes in settings
    SettingsVersion version{0U};

    bool operator==(const ConnectionSettings& rhs) const {
        return !RequiresConnectionReset(rhs) && recent_errors_threshold == rhs.recent_errors_threshold;
    }

    bool operator!=(const ConnectionSettings& rhs) const { return !(*this == rhs); }

    bool RequiresConnectionReset(const ConnectionSettings& rhs) const {
        // TODO: max_prepared_cache_size check could be relaxed
        return prepared_statements != rhs.prepared_statements || user_types != rhs.user_types ||
               ignore_unused_query_params != rhs.ignore_unused_query_params ||
               max_prepared_cache_size != rhs.max_prepared_cache_size || pipeline_mode != rhs.pipeline_mode ||
               max_ttl != rhs.max_ttl || discard_on_connect != rhs.discard_on_connect ||
               omit_describe_mode != rhs.omit_describe_mode;
    }
};

struct ConnectionSettingsDynamic final {
    std::optional<ConnectionSettings::PreparedStatementOptions> prepared_statements{};
    std::optional<ConnectionSettings::UserTypesOptions> user_types{};
    std::optional<std::size_t> max_prepared_cache_size{};
    std::optional<std::size_t> recent_errors_threshold{};
    std::optional<ConnectionSettings::CheckQueryParamsOptions> ignore_unused_query_params{};
    std::optional<std::chrono::seconds> max_ttl{};
    std::optional<ConnectionSettings::DiscardOnConnectOptions> discard_on_connect{};
    std::optional<bool> deadline_propagation_enabled{};
};

/// @brief PostgreSQL statements metrics options
///
/// Dynamic option @ref POSTGRES_STATEMENT_METRICS_SETTINGS
struct StatementMetricsSettings final {
    /// Store metrics in LRU of this size
    std::size_t max_statements{0};

    bool operator==(const StatementMetricsSettings& other) const { return max_statements == other.max_statements; }
};

/// Initialization modes
enum class InitMode {
    kSync = 0,
    kAsync,
};

enum class ConnlimitMode {
    kManual = 0,
    kAuto,
};

/// Settings for storages::postgres::Cluster
struct ClusterSettings {
    /// settings for statements metrics
    StatementMetricsSettings statement_metrics_settings;

    /// settings for host discovery
    TopologySettings topology_settings;

    /// settings for connection pools
    PoolSettings pool_settings;

    /// settings for individual connections
    ConnectionSettings conn_settings;

    /// initialization mode
    InitMode init_mode = InitMode::kSync;

    /// database name
    std::string db_name;

    /// connection limit change mode
    ConnlimitMode connlimit_mode = ConnlimitMode::kAuto;

    /// congestion control settings
    congestion_control::v2::LinearController::StaticConfig cc_config;
};

}  // namespace storages::postgres

USERVER_NAMESPACE_END
