#include <userver/ugrpc/client/impl/call_state.hpp>

#include <dynamic_config/variables/USERVER_GRPC_CLIENT_ENABLE_DEADLINE_PROPAGATION.hpp>

#include <userver/utils/assert.hpp>

#include <userver/ugrpc/client/impl/call_params.hpp>

#include <ugrpc/client/impl/tracing.hpp>

USERVER_NAMESPACE_BEGIN

namespace ugrpc::client::impl {

RpcConfigValues::RpcConfigValues(const dynamic_config::Snapshot& config)
    : enforce_task_deadline(config[::dynamic_config::USERVER_GRPC_CLIENT_ENABLE_DEADLINE_PROPAGATION]) {}

CallState::CallState(CallParams&& params, CallKind call_kind)
    : stub_(std::move(params.stub)),
      client_context_(std::move(params.context)),
      client_name_(params.client_name),
      call_name_(std::move(params.call_name)),
      stats_scope_(params.statistics),
      queue_(params.queue),
      config_values_(params.config),
      middlewares_(params.middlewares),
      call_kind_(call_kind) {
    UASSERT(client_context_);
    UASSERT(!client_name_.empty());
    SetupSpan(span_, *client_context_, call_name_.Get());
}

CallState::~CallState() noexcept {
    invocation_.emplace<std::monostate>();

    if (client_context_ && !IsFinished()) {
        client_context_->TryCancel();

        UASSERT(span_);
        SetErrorForSpan(GetSpan(), "Abandoned");
        ResetSpan();
    }
}

StubHandle& CallState::GetStub() noexcept { return stub_; }

const grpc::ClientContext& CallState::GetClientContext() const noexcept {
    UASSERT(client_context_);
    return *client_context_;
}

grpc::ClientContext& CallState::GetClientContext() noexcept {
    UASSERT(client_context_);
    return *client_context_;
}

grpc::CompletionQueue& CallState::GetQueue() const noexcept {
    UASSERT(client_context_);
    return queue_;
}

const RpcConfigValues& CallState::GetConfigValues() const noexcept {
    UASSERT(client_context_);
    return config_values_;
}

const Middlewares& CallState::GetMiddlewares() const noexcept {
    UASSERT(client_context_);
    return middlewares_;
}

std::string_view CallState::GetCallName() const noexcept {
    UASSERT(client_context_);
    return call_name_.Get();
}

std::string_view CallState::GetClientName() const noexcept {
    UASSERT(client_context_);
    return client_name_;
}

tracing::Span& CallState::GetSpan() noexcept {
    UASSERT(client_context_);
    UASSERT(span_);
    return span_->Get();
}

CallKind CallState::GetCallKind() const noexcept { return call_kind_; }

void CallState::ResetSpan() noexcept {
    UASSERT(client_context_);
    UASSERT(span_);
    span_.reset();
}

ugrpc::impl::RpcStatisticsScope& CallState::GetStatsScope() noexcept {
    UASSERT(client_context_);
    return stats_scope_;
}

void CallState::SetFinished() noexcept {
    UASSERT(client_context_);
    UINVARIANT(!is_finished_, "Tried to finish an already finished call");
    is_finished_ = true;
}

bool CallState::IsFinished() const noexcept {
    UASSERT(client_context_);
    return is_finished_;
}

void CallState::SetDeadlinePropagated() noexcept {
    UASSERT(client_context_);
    stats_scope_.OnDeadlinePropagated();
    is_deadline_propagated_ = true;
}

bool CallState::IsDeadlinePropagated() const noexcept {
    UASSERT(client_context_);
    return is_deadline_propagated_;
}

void CallState::SetWritesFinished() noexcept {
    UASSERT(client_context_);
    UASSERT(!writes_finished_);
    writes_finished_ = true;
}

bool CallState::AreWritesFinished() const noexcept {
    UASSERT(client_context_);
    return writes_finished_;
}

bool CallState::IsReadAvailable() const noexcept { return !IsFinished(); }

bool CallState::IsWriteAvailable() const noexcept { return !AreWritesFinished(); }

bool CallState::IsWriteAndCheckAvailable() const noexcept { return !AreWritesFinished() && !IsFinished(); }

void CallState::EmplaceAsyncMethodInvocation() {
    UINVARIANT(
        std::holds_alternative<std::monostate>(invocation_),
        "Another method is already running for this RPC concurrently"
    );
    invocation_.emplace<AsyncMethodInvocation>();
}

void CallState::EmplaceFinishAsyncMethodInvocation() {
    UINVARIANT(
        std::holds_alternative<std::monostate>(invocation_),
        "Another method is already running for this RPC concurrently"
    );
    invocation_.emplace<FinishAsyncMethodInvocation>();
}

AsyncMethodInvocation& CallState::GetAsyncMethodInvocation() noexcept {
    UASSERT(std::holds_alternative<AsyncMethodInvocation>(invocation_));
    return std::get<AsyncMethodInvocation>(invocation_);
}

FinishAsyncMethodInvocation& CallState::GetFinishAsyncMethodInvocation() noexcept {
    UASSERT(std::holds_alternative<FinishAsyncMethodInvocation>(invocation_));
    return std::get<FinishAsyncMethodInvocation>(invocation_);
}

bool CallState::HoldsAsyncMethodInvocationDebug() noexcept {
    return std::holds_alternative<AsyncMethodInvocation>(invocation_);
}

bool CallState::HoldsFinishAsyncMethodInvocationDebug() noexcept {
    return std::holds_alternative<FinishAsyncMethodInvocation>(invocation_);
}

bool CallState::IsFinishProcessed() const noexcept { return finish_processed_; }

void CallState::SetFinishProcessed() noexcept {
    UASSERT(!finish_processed_);
    finish_processed_ = true;
}

bool CallState::IsStatusExtracted() const noexcept { return status_extracted_; }

void CallState::SetStatusExtracted() noexcept {
    UASSERT(!status_extracted_);
    status_extracted_ = true;
}

grpc::Status& CallState::GetStatus() noexcept { return status_; }

CallState::AsyncMethodInvocationGuard::AsyncMethodInvocationGuard(CallState& state) noexcept : state_(state) {
    UASSERT(!std::holds_alternative<std::monostate>(state_.invocation_));
}

CallState::AsyncMethodInvocationGuard::~AsyncMethodInvocationGuard() noexcept {
    UASSERT(!std::holds_alternative<std::monostate>(state_.invocation_));
    if (!disarm_) {
        state_.invocation_.emplace<std::monostate>();
    }
}

}  // namespace ugrpc::client::impl

USERVER_NAMESPACE_END
