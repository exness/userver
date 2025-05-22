#include <userver/ugrpc/client/impl/async_methods.hpp>

#include <fmt/format.h>

#include <userver/tracing/tags.hpp>
#include <userver/utils/algo.hpp>

#include <ugrpc/client/impl/tracing.hpp>
#include <userver/ugrpc/client/exceptions.hpp>
#include <userver/ugrpc/impl/statistics_scope.hpp>

USERVER_NAMESPACE_BEGIN

namespace ugrpc::client::impl {

void CheckOk(CallState& state, AsyncMethodInvocation::WaitStatus status, std::string_view stage) {
    if (status == impl::AsyncMethodInvocation::WaitStatus::kError) {
        state.SetFinished();
        state.GetStatsScope().OnNetworkError();
        state.GetStatsScope().Flush();
        SetErrorAndResetSpan(state, fmt::format("Network error at '{}'", stage));
        throw RpcInterruptedError(state.GetCallName(), stage);
    } else if (status == impl::AsyncMethodInvocation::WaitStatus::kCancelled) {
        state.SetFinished();
        state.GetStatsScope().OnCancelled();
        SetErrorAndResetSpan(state, fmt::format("Network error at '{}' (task cancelled)", stage));
        throw RpcCancelledError(state.GetCallName(), stage);
    }
}

void PrepareFinish(CallState& state) {
    UINVARIANT(!state.IsFinished(), "'Finish' called on a finished call");
    state.SetFinished();
}

void ProcessFinish(
    CallState& state,
    utils::function_ref<void(CallState& state, const grpc::Status& status)> post_finish
) {
    const auto& status = state.GetStatus();

    state.GetStatsScope().OnExplicitFinish(status.error_code());
    state.GetStatsScope().Flush();

    post_finish(state, status);

    SetStatusAndResetSpan(state, status);
}

void CheckFinishStatus(CallState& state) {
    auto& status = state.GetStatus();
    if (!status.ok()) {
        ThrowErrorWithStatus(state.GetCallName(), std::move(status));
    }
}

void ProcessFinishResult(
    CallState& state,
    AsyncMethodInvocation::WaitStatus wait_status,
    utils::function_ref<void(CallState& state, const grpc::Status& status)> post_finish,
    bool throw_on_error
) {
    const auto ok = wait_status == impl::AsyncMethodInvocation::WaitStatus::kOk;
    UASSERT_MSG(
        ok,
        "ok=false in async Finish method invocation is prohibited "
        "by gRPC docs, see grpc::CompletionQueue::Next"
    );

    ProcessFinish(state, post_finish);

    if (throw_on_error) {
        CheckFinishStatus(state);
    }
}

}  // namespace ugrpc::client::impl

USERVER_NAMESPACE_END
