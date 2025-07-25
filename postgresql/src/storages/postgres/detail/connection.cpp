#include <storages/postgres/detail/connection.hpp>

#include <storages/postgres/detail/connection_impl.hpp>
#include <userver/clients/dns/exception.hpp>

USERVER_NAMESPACE_BEGIN

namespace storages::postgres::detail {
namespace {

constexpr TimeoutDuration kMinConnectTimeout = std::chrono::seconds{2};

}  // namespace

Connection::Connection() = default;

Connection::~Connection() = default;

std::unique_ptr<Connection> Connection::Connect(
    const Dsn& dsn,
    clients::dns::Resolver* resolver,
    engine::TaskProcessor& bg_task_processor,
    concurrent::BackgroundTaskStorageCore& bg_task_storage,
    uint32_t id,
    ConnectionSettings settings,
    const DefaultCommandControls& default_cmd_ctls,
    const testsuite::PostgresControl& testsuite_pg_ctl,
    const error_injection::Settings& ei_settings,
    engine::SemaphoreLock&& size_lock,
    USERVER_NAMESPACE::utils::statistics::MetricsStoragePtr metrics
) {
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
    std::unique_ptr<Connection> conn(new Connection());

    const auto deadline = engine::Deadline::FromDuration(
        std::max(kMinConnectTimeout, default_cmd_ctls.GetDefaultCmdCtl().network_timeout_ms)
    );
    conn->pimpl_ = std::make_unique<ConnectionImpl>(
        bg_task_processor,
        bg_task_storage,
        id,
        settings,
        default_cmd_ctls,
        testsuite_pg_ctl,
        ei_settings,
        std::move(size_lock),
        std::move(metrics)
    );
    if (resolver) {
        try {
            conn->pimpl_->AsyncConnect(ResolveDsnHostaddrs(dsn, *resolver, deadline), deadline);
        } catch (const std::exception& ex) {
            throw ConnectionError{ex.what()};
        }
    } else {
        conn->pimpl_->AsyncConnect(dsn, deadline);
    }

    return conn;
}

void Connection::Close() { pimpl_->Close(); }

bool Connection::IsInAbortedPipeline() const { return pimpl_->IsInAbortedPipeline(); }

bool Connection::IsInRecovery() const { return pimpl_->IsInRecovery(); }

bool Connection::IsReadOnly() const { return pimpl_->IsReadOnly(); }

void Connection::RefreshReplicaState(engine::Deadline deadline) const { pimpl_->RefreshReplicaState(deadline); }

const ConnectionSettings& Connection::GetSettings() const { return pimpl_->GetSettings(); }

ConnectionState Connection::GetState() const { return pimpl_->GetConnectionState(); }

bool Connection::IsConnected() const { return pimpl_->IsConnected(); }

bool Connection::IsIdle() const { return pimpl_->IsIdle(); }

bool Connection::IsBroken() const { return pimpl_->IsBroken(); }

bool Connection::IsExpired() const { return pimpl_->IsExpired(); }

bool Connection::IsPipelineActive() const { return pimpl_->IsPipelineActive(); }

bool Connection::ArePreparedStatementsEnabled() const { return pimpl_->ArePreparedStatementsEnabled(); }

int Connection::GetServerVersion() const { return pimpl_->GetServerVersion(); }

bool Connection::IsInTransaction() const { return pimpl_->IsInTransaction(); }

CommandControl Connection::GetDefaultCommandControl() const { return pimpl_->GetDefaultCommandControl(); }

void Connection::UpdateDefaultCommandControl() { pimpl_->UpdateDefaultCommandControl(); }

Connection::Statistics Connection::GetStatsAndReset() { return pimpl_->GetStatsAndReset(); }

void Connection::Begin(
    const TransactionOptions& options,
    SteadyClock::time_point trx_start_time,
    OptionalCommandControl trx_cmd_ctl
) {
    pimpl_->Begin(options, trx_start_time, std::move(trx_cmd_ctl));
}

void Connection::Commit() { pimpl_->Commit(); }

void Connection::Rollback() { pimpl_->Rollback(); }

void Connection::Start(SteadyClock::time_point start_time) { pimpl_->Start(start_time); }

void Connection::Finish() { pimpl_->Finish(); }

ResultSet Connection::Execute(
    const Query& query,
    const detail::QueryParameters& params,
    OptionalCommandControl statement_cmd_ctl
) {
    return pimpl_->ExecuteCommand(query, params, std::move(statement_cmd_ctl));
}

Connection::PreparedStatementMeta
Connection::PrepareStatement(const Query& query, const detail::QueryParameters& params, TimeoutDuration timeout) {
    const auto& statement_info = pimpl_->PrepareStatement(query, params, timeout);

    return {statement_info.statement_name, statement_info.description};
}

void Connection::AddIntoPipeline(
    CommandControl cc,
    const std::string& prepared_statement_name,
    const detail::QueryParameters& params,
    const ResultSet& description,
    tracing::ScopeTime& scope
) {
    pimpl_->AddIntoPipeline(cc, prepared_statement_name, params, description, scope);
}

std::vector<ResultSet> Connection::GatherPipeline(TimeoutDuration timeout, const std::vector<ResultSet>& descriptions) {
    return pimpl_->GatherPipeline(timeout, descriptions);
}

ResultSet Connection::Execute(const Query& query, const ParameterStore& store) {
    return Execute(query, detail::QueryParameters{store.GetInternalData()});
}

ResultSet Connection::Execute(CommandControl statement_cmd_ctl, const Query& query, const ParameterStore& store) {
    return Execute(query, detail::QueryParameters{store.GetInternalData()}, OptionalCommandControl{statement_cmd_ctl});
}

Connection::StatementId Connection::PortalBind(
    USERVER_NAMESPACE::utils::zstring_view statement,
    const std::string& portal_name,
    const detail::QueryParameters& params,
    OptionalCommandControl statement_cmd_ctl
) {
    return pimpl_->PortalBind(statement, portal_name, params, std::move(statement_cmd_ctl));
}

ResultSet Connection::PortalExecute(
    StatementId statement_id,
    const std::string& portal_name,
    std::uint32_t n_rows,
    OptionalCommandControl statement_cmd_ctl
) {
    return pimpl_->PortalExecute(statement_id, portal_name, n_rows, std::move(statement_cmd_ctl));
}

void Connection::CancelAndCleanup(TimeoutDuration timeout) { pimpl_->CancelAndCleanup(timeout); }

bool Connection::Cleanup(TimeoutDuration timeout) { return pimpl_->Cleanup(timeout); }

void Connection::SetParameter(const std::string& param, const std::string& value, ParameterScope scope) {
    pimpl_->SetParameter(param, value, scope);
}

void Connection::ReloadUserTypes() { pimpl_->LoadUserTypes(); }

const UserTypes& Connection::GetUserTypes() const { return pimpl_->GetUserTypes(); }

void Connection::Listen(std::string_view channel, OptionalCommandControl cmd_ctl) { pimpl_->Listen(channel, cmd_ctl); }

void Connection::Unlisten(std::string_view channel, OptionalCommandControl cmd_ctl) {
    pimpl_->Unlisten(channel, cmd_ctl);
}

Notification Connection::WaitNotify(engine::Deadline deadline) { return pimpl_->WaitNotify(deadline); }

TimeoutDuration Connection::GetIdleDuration() const { return pimpl_->GetIdleDuration(); }

void Connection::Ping() { pimpl_->Ping(); }

void Connection::MarkAsBroken() { pimpl_->MarkAsBroken(); }

OptionalCommandControl Connection::GetQueryCmdCtl(std::optional<Query::NameView> query_name) const {
    return pimpl_->GetNamedQueryCommandControl(query_name);
}

const OptionalCommandControl& Connection::GetTransactionCommandControl() const {
    return pimpl_->GetTransactionCommandControl();
}

TimeoutDuration Connection::GetStatementTimeout() const { return pimpl_->GetStatementTimeout(); }

}  // namespace storages::postgres::detail

USERVER_NAMESPACE_END
