#include <storages/postgres/connlimit_watchdog.hpp>

#include <storages/postgres/detail/cluster_impl.hpp>
#include <userver/utils/from_string.hpp>

USERVER_NAMESPACE_BEGIN

namespace storages::postgres {

namespace {
constexpr CommandControl kCommandControl{std::chrono::seconds(2), std::chrono::seconds(2)};
constexpr size_t kTestsuiteConnlimit = 100;
constexpr size_t kReservedConn = 5;

constexpr int kMaxStepsWithError = 3;
constexpr size_t kFallbackConnlimit = 20;

}  // namespace

ConnlimitWatchdog::ConnlimitWatchdog(
    detail::ClusterImpl& cluster,
    testsuite::TestsuiteTasks& testsuite_tasks,
    int shard_number,
    std::function<void()> on_new_connlimit,
    std::string host_name
)
    : cluster_(cluster),
      connlimit_(0),
      on_new_connlimit_(std::move(on_new_connlimit)),
      testsuite_tasks_(testsuite_tasks),
      shard_number_(shard_number),
      host_name_(std::move(host_name)) {}

void ConnlimitWatchdog::Start() {
    try {
        auto trx = cluster_.Begin({ClusterHostType::kMaster}, {}, kCommandControl);
        trx.Execute(R"(
          CREATE TABLE IF NOT EXISTS u_clients (
              hostname TEXT PRIMARY KEY,
              updated TIMESTAMPTZ NOT NULL,
              max_connections INTEGER NOT NULL
          );
        )");
        trx.Execute("ALTER TABLE u_clients ADD COLUMN IF NOT EXISTS cur_user TEXT");
        // Beware! Do **not** change queries in StepV*, but rather provide a new StepV* to avoid migration issues.
        trx.Commit();
    } catch (const storages::postgres::AccessRuleViolation& e) {
        // Possible in some CREATE TABLE IF NOT EXISTS races with other services
        LOG_WARNING() << "Table already exists (not a fatal error): " << e;
    } catch (const storages::postgres::UniqueViolation& e) {
        // Possible in some CREATE TABLE IF NOT EXISTS races with other services
        LOG_WARNING() << "Table already exists (not a fatal error): " << e;
    }

    if (testsuite_tasks_.IsEnabled()) {
        connlimit_ = kTestsuiteConnlimit;
        testsuite_tasks_.RegisterTask(
            fmt::format("connlimit_watchdog_{}_{}", cluster_.GetDbName(), shard_number_), [this] { StepV1(); }
        );
    } else {
        periodic_.Start(
            "connlimit_watchdog",
            {std::chrono::seconds(2), {}, {USERVER_NAMESPACE::utils::PeriodicTask::Flags::kNow}},
            [this] { StepV2(); }
        );
    }
}

void ConnlimitWatchdog::StepV1() {
    static auto kHostname = hostinfo::blocking::GetRealHostName();
    try {
        auto trx = cluster_.Begin({ClusterHostType::kMaster}, {}, kCommandControl);

        auto max_connections1 = USERVER_NAMESPACE::utils::FromString<ssize_t>(
            trx.Execute("SHOW max_connections;").AsSingleRow<std::string>()
        );
        auto max_connections2 =
            trx.Execute("SELECT rolconnlimit FROM pg_roles WHERE rolname = current_user").AsSingleRow<ssize_t>();
        if (max_connections2 < 0) max_connections2 = max_connections1;
        size_t max_connections = std::min(max_connections1, max_connections2);

        if (max_connections > kReservedConn)
            max_connections -= kReservedConn;
        else
            max_connections = 1;

        trx.Execute(
            "INSERT INTO u_clients (hostname, updated, max_connections) VALUES "
            "($1, "
            "NOW(), $2) ON CONFLICT (hostname) DO UPDATE SET updated = NOW(), "
            "max_connections = $2",
            kHostname,
            static_cast<int>(GetConnlimit())
        );
        auto instances = trx.Execute(
                                "SELECT count(*) FROM u_clients WHERE updated >= "
                                "NOW() - make_interval(secs => 15)"
        )
                             .AsSingleRow<int>();
        if (instances == 0) instances = 1;

        auto connlimit = max_connections / instances;
        if (connlimit == 0) connlimit = 1;
        LOG((connlimit_ == connlimit) ? logging::Level::kDebug : logging::Level::kWarning)
            << "max_connections = " << max_connections << ", instances = " << instances
            << ", connlimit = " << connlimit;
        connlimit_ = connlimit;

        trx.Commit();
        steps_with_errors_ = 0;
    } catch (const Error& e) {
        if (++steps_with_errors_ > kMaxStepsWithError) {
            /*
             * Something's wrong with PG server. Try to lower the load by lowering
             * max connection to a small value. Active connections will be gracefully
             * closed. When the server returns the response, we'll get the real
             * connlimit value. The period with "too low max_connections" should be
             * relatively small.
             */
            connlimit_ = kFallbackConnlimit;
        }
    }

    on_new_connlimit_();
}

void ConnlimitWatchdog::StepV2() {
    static auto kHostname = hostinfo::blocking::GetRealHostName();
    try {
        auto trx = cluster_.Begin({ClusterHostType::kMaster}, {}, kCommandControl);

        auto max_connections1 = USERVER_NAMESPACE::utils::FromString<ssize_t>(
            trx.Execute("SHOW max_connections;").AsSingleRow<std::string>()
        );
        auto max_connections2 =
            trx.Execute("SELECT rolconnlimit FROM pg_roles WHERE rolname = current_user").AsSingleRow<ssize_t>();
        if (max_connections2 < 0) max_connections2 = max_connections1;
        size_t max_connections = std::min(max_connections1, max_connections2);

        if (max_connections > kReservedConn)
            max_connections -= kReservedConn;
        else
            max_connections = 1;

        trx.Execute(
            R"(
              INSERT INTO u_clients (hostname, updated, max_connections, cur_user) VALUES
              ($1, NOW(), $2, current_user) ON CONFLICT (hostname) DO UPDATE SET updated = NOW(), max_connections = $2, cur_user = current_user
            )",
            host_name_,
            static_cast<int>(GetConnlimit())
        );

        auto instances =
            trx.Execute(
                   R"(SELECT count(*) FROM u_clients WHERE updated >= NOW() - make_interval(secs => 15) AND (cur_user = current_user OR cur_user is NULL))"
            )
                .AsSingleRow<int>();
        if (instances == 0) instances = 1;

        auto connlimit = max_connections / instances;
        if (connlimit == 0) connlimit = 1;
        LOG((connlimit_ == connlimit) ? logging::Level::kDebug : logging::Level::kWarning)
            << "max_connections = " << max_connections << ", instances = " << instances
            << ", connlimit = " << connlimit;
        connlimit_ = connlimit;

        trx.Commit();
        steps_with_errors_ = 0;
    } catch (const Error& e) {
        if (++steps_with_errors_ > kMaxStepsWithError) {
            /*
             * Something's wrong with PG server. Try to lower the load by lowering
             * max connection to a small value. Active connections will be gracefully
             * closed. When the server returns the response, we'll get the real
             * connlimit value. The period with "too low max_connections" should be
             * relatively small.
             */
            connlimit_ = kFallbackConnlimit;
        }
    }

    on_new_connlimit_();
}

void ConnlimitWatchdog::Stop() { periodic_.Stop(); }

size_t ConnlimitWatchdog::GetConnlimit() const { return connlimit_.load(); }

}  // namespace storages::postgres

USERVER_NAMESPACE_END
