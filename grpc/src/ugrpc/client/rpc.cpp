#include <userver/ugrpc/client/rpc.hpp>

#include <userver/engine/task/cancel.hpp>

#include <userver/ugrpc/client/exceptions.hpp>
#include <userver/ugrpc/client/middlewares/base.hpp>

USERVER_NAMESPACE_BEGIN

namespace ugrpc::client::impl {

UnaryFinishFutureImpl::UnaryFinishFutureImpl(impl::CallState& state, const google::protobuf::Message* response) noexcept
    : state_(&state), response_(response) {
    // We expect that FinishAsyncMethodInvocation was already emplaced
    // For unary future it is done in UnaryCall::FinishAsync
    UASSERT(state_->HoldsFinishAsyncMethodInvocationDebug());
}

UnaryFinishFutureImpl::UnaryFinishFutureImpl(UnaryFinishFutureImpl&& other) noexcept
    // state_ == nullptr signals that *this is empty. Other fields may remain garbage in `other`.
    : state_{std::exchange(other.state_, nullptr)},
      response_(other.response_),
      exception_(std::move(other.exception_)) {}

UnaryFinishFutureImpl& UnaryFinishFutureImpl::operator=(UnaryFinishFutureImpl&& other) noexcept {
    if (this == &other) return *this;
    // we should destruct current instance
    [[maybe_unused]] auto for_destruction = std::move(*this);
    // state_ == nullptr signals that *this is empty. Other fields may remain garbage in `other`.
    state_ = std::exchange(other.state_, nullptr);
    response_ = other.response_;
    exception_ = std::move(other.exception_);
    return *this;
}

UnaryFinishFutureImpl::~UnaryFinishFutureImpl() { Destroy(); }

void UnaryFinishFutureImpl::Destroy() noexcept try {
    if (!state_) {
        return;
    }
    auto& state = *state_;
    if (state.IsFinishProcessed()) {
        return;
    }
    state.SetFinishProcessed();
    state.GetContext().TryCancel();
    auto& finish = state.GetFinishAsyncMethodInvocation();

    const engine::TaskCancellationBlocker cancel_blocker;
    const auto wait_status = finish.Wait();

    state.GetStatsScope().SetFinishTime(finish.GetFinishTime());

    switch (wait_status) {
        case impl::AsyncMethodInvocation::WaitStatus::kOk:
            ProcessFinishAbandoned(state);
            break;
        case impl::AsyncMethodInvocation::WaitStatus::kError:
            ProcessFinishNetworkError(state);
            break;
        case impl::AsyncMethodInvocation::WaitStatus::kCancelled:
        case impl::AsyncMethodInvocation::WaitStatus::kDeadline:
            utils::AbortWithStacktrace("unreachable");
    }
} catch (const std::exception& ex) {
    LOG_WARNING() << "There is a caught exception in 'UnaryFinishFutureImpl::Destroy': " << ex;
}

bool UnaryFinishFutureImpl::IsReady() const noexcept {
    UASSERT_MSG(state_, "'IsReady' called on a moved out future");
    auto& finish = state_->GetFinishAsyncMethodInvocation();
    return finish.IsReady();
}

engine::FutureStatus UnaryFinishFutureImpl::WaitUntil(engine::Deadline deadline) const noexcept {
    UASSERT_MSG(state_, "'WaitUntil' called on a moved out future");
    if (!state_) return engine::FutureStatus::kReady;

    if (state_->IsFinishProcessed()) return engine::FutureStatus::kReady;

    auto& finish = state_->GetFinishAsyncMethodInvocation();
    const auto wait_status = impl::WaitAndTryCancelIfNeeded(finish, deadline, state_->GetContext());
    switch (wait_status) {
        case impl::AsyncMethodInvocation::WaitStatus::kOk:
            state_->SetFinishProcessed();
            state_->GetStatsScope().SetFinishTime(finish.GetFinishTime());
            try {
                ProcessFinish(*state_, response_);
            } catch (...) {
                exception_ = std::current_exception();
            }
            return engine::FutureStatus::kReady;

        case impl::AsyncMethodInvocation::WaitStatus::kError:
            state_->SetFinishProcessed();
            state_->GetStatsScope().SetFinishTime(finish.GetFinishTime());
            ProcessFinishNetworkError(*state_);
            exception_ = std::make_exception_ptr(RpcInterruptedError(state_->GetCallName(), "Finish"));
            return engine::FutureStatus::kReady;

        case impl::AsyncMethodInvocation::WaitStatus::kCancelled:
            state_->GetStatsScope().OnCancelled();
            return engine::FutureStatus::kCancelled;

        case impl::AsyncMethodInvocation::WaitStatus::kDeadline:
            return engine::FutureStatus::kTimeout;
    }

    utils::AbortWithStacktrace("should be unreachable");
}

void UnaryFinishFutureImpl::Get() {
    UINVARIANT(state_, "'Get' called on a moved out future");
    UINVARIANT(!state_->IsStatusExtracted(), "'Get' called multiple times on the same future");
    state_->SetStatusExtracted();

    const auto future_status = WaitUntil(engine::Deadline{});

    if (future_status == engine::FutureStatus::kCancelled) {
        throw RpcCancelledError(state_->GetCallName(), "UnaryFuture::Get");
    }
    UASSERT(state_->IsFinishProcessed());

    if (exception_) {
        std::rethrow_exception(std::exchange(exception_, {}));
    }

    CheckFinishStatus(*state_);
}

engine::impl::ContextAccessor* UnaryFinishFutureImpl::TryGetContextAccessor() noexcept {
    // Unfortunately, we can't require that TryGetContextAccessor is not called
    // after future is finished - it doesn't match pattern usage of WaitAny
    // Instead we should return nullptr
    if (!state_ || state_->IsStatusExtracted()) {
        return nullptr;
    }

    // if state exists, then FinishAsyncMethodInvocation also exists
    auto& finish = state_->GetFinishAsyncMethodInvocation();
    return finish.TryGetContextAccessor();
}

}  // namespace ugrpc::client::impl

USERVER_NAMESPACE_END
