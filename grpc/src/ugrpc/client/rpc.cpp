#include <userver/ugrpc/client/response_future.hpp>

#include <userver/engine/task/cancel.hpp>

#include <userver/ugrpc/client/exceptions.hpp>
#include <userver/ugrpc/client/middlewares/base.hpp>

USERVER_NAMESPACE_BEGIN

namespace ugrpc::client::impl {

UnaryFinishFutureImpl::UnaryFinishFutureImpl(
    impl::RpcData& data,
    std::function<void(impl::RpcData& data, const grpc::Status& status)> post_finish
) noexcept
    : data_(&data), post_finish_(std::move(post_finish)) {
    // We expect that FinishAsyncMethodInvocation was already emplaced
    // For unary future it is done in UnaryCall::FinishAsync
    UASSERT(data_->HoldsFinishAsyncMethodInvocationDebug());
}

UnaryFinishFutureImpl::UnaryFinishFutureImpl(UnaryFinishFutureImpl&& other) noexcept
    : data_{std::exchange(other.data_, nullptr)},
      post_finish_{std::move(other.post_finish_)},
      exception_(std::move(other.exception_)) {}

UnaryFinishFutureImpl& UnaryFinishFutureImpl::operator=(UnaryFinishFutureImpl&& other) noexcept {
    if (this == &other) return *this;
    [[maybe_unused]] auto for_destruction = std::move(*this);
    data_ = std::exchange(other.data_, nullptr);
    post_finish_ = std::move(other.post_finish_);
    exception_ = std::move(other.exception_);
    return *this;
}

UnaryFinishFutureImpl::~UnaryFinishFutureImpl() {
    if (data_ && !data_->IsFinishProcessed()) {
        data_->GetContext().TryCancel();

        const engine::TaskCancellationBlocker cancel_blocker;
        const auto future_status = WaitUntil(engine::Deadline{});
        UASSERT(future_status == engine::FutureStatus::kReady);

        UASSERT(data_->IsFinishProcessed());
    }
}

bool UnaryFinishFutureImpl::IsReady() const noexcept {
    UASSERT_MSG(data_, "'IsReady' called on a moved out future");
    auto& finish = data_->GetFinishAsyncMethodInvocation();
    return finish.IsReady();
}

engine::FutureStatus UnaryFinishFutureImpl::WaitUntil(engine::Deadline deadline) const noexcept {
    UASSERT_MSG(data_, "'WaitUntil' called on a moved out future");
    if (!data_) return engine::FutureStatus::kReady;

    if (data_->IsFinishProcessed()) return engine::FutureStatus::kReady;

    auto& finish = data_->GetFinishAsyncMethodInvocation();
    const auto wait_status = impl::WaitUntil(finish, data_->GetContext(), deadline);

    switch (wait_status) {
        case impl::AsyncMethodInvocation::WaitStatus::kOk:
        case impl::AsyncMethodInvocation::WaitStatus::kError:
            data_->SetFinishProcessed();
            try {
                UINVARIANT(
                    impl::AsyncMethodInvocation::WaitStatus::kOk == wait_status,
                    "Client-side Finish: ok should always be true"
                );
                ProcessFinish(*data_, post_finish_);
            } catch (...) {
                exception_ = std::current_exception();
            }
            return engine::FutureStatus::kReady;

        case impl::AsyncMethodInvocation::WaitStatus::kCancelled:
            data_->GetStatsScope().OnCancelled();
            return engine::FutureStatus::kCancelled;

        case impl::AsyncMethodInvocation::WaitStatus::kDeadline:
            return engine::FutureStatus::kTimeout;
    }

    utils::AbortWithStacktrace("should be unreachable");
}

void UnaryFinishFutureImpl::Get() {
    UINVARIANT(data_, "'Get' called on a moved out future");
    UINVARIANT(!data_->IsStatusExtracted(), "'Get' called multiple times on the same future");
    data_->SetStatusExtracted();

    const auto future_status = WaitUntil(engine::Deadline{});

    if (future_status == engine::FutureStatus::kCancelled) {
        throw RpcCancelledError(data_->GetCallName(), "UnaryFuture::Get");
    }
    UASSERT(data_->IsFinishProcessed());

    if (exception_) {
        std::rethrow_exception(std::exchange(exception_, {}));
    }

    CheckFinishStatus(*data_);
}

engine::impl::ContextAccessor* UnaryFinishFutureImpl::TryGetContextAccessor() noexcept {
    // Unfortunately, we can't require that TryGetContextAccessor is not called
    // after future is finished - it doesn't match pattern usage of WaitAny
    // Instead we should return nullptr
    if (!data_ || data_->IsStatusExtracted()) {
        return nullptr;
    }

    // if data exists, then FinishAsyncMethodInvocation also exists
    auto& finish = data_->GetFinishAsyncMethodInvocation();
    return finish.TryGetContextAccessor();
}

}  // namespace ugrpc::client::impl

USERVER_NAMESPACE_END
