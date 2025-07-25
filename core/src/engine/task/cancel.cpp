#include <userver/engine/task/cancel.hpp>

#include <exception>

#include <fmt/format.h>
#include <boost/algorithm/string/replace.hpp>

#include <userver/engine/sleep.hpp>
#include <userver/logging/log.hpp>
#include <userver/utils/assert.hpp>

#include <engine/task/coro_unwinder.hpp>
#include <engine/task/task_base_impl.hpp>
#include <engine/task/task_context.hpp>

USERVER_NAMESPACE_BEGIN

namespace engine {
namespace {

void Unwind() {
    auto& ctx = current_task::GetCurrentTaskContext();
    UASSERT(ctx.GetState() == Task::State::kRunning);

    if (std::uncaught_exceptions()) return;

    if (ctx.SetCancellable(false)) {
        LOG_TRACE() << "Cancelling current task" << logging::LogExtra::Stacktrace();
        // NOLINTNEXTLINE(hicpp-exception-baseclass)
        throw impl::CoroUnwinder{};
    }
}

}  // namespace

namespace current_task {

bool IsCancelRequested() noexcept {
    // Current task is running, so we do not get scheduled and no exception could
    // happen
    return GetCurrentTaskContext().IsCancelRequested();
}

bool ShouldCancel() noexcept {
    // Current task is running, so we do not get scheduled and no exception
    // could happen
    return GetCurrentTaskContext().ShouldCancel();
}

TaskCancellationReason CancellationReason() noexcept { return GetCurrentTaskContext().CancellationReason(); }

void CancellationPoint() {
    if (current_task::ShouldCancel()) Unwind();
}

void SetDeadline(Deadline deadline) { GetCurrentTaskContext().SetCancelDeadline(deadline); }

TaskCancellationToken GetCancellationToken() { return TaskCancellationToken(GetCurrentTaskContext()); }

void RequestCancel() { return GetCurrentTaskContext().RequestCancel(TaskCancellationReason::kUserRequest); }

}  // namespace current_task

TaskCancellationBlocker::TaskCancellationBlocker()
    : context_(current_task::GetCurrentTaskContext()), was_allowed_(context_.SetCancellable(false)) {}

TaskCancellationBlocker::~TaskCancellationBlocker() {
    UASSERT(context_.IsCurrent());
    context_.SetCancellable(was_allowed_);
}

std::string_view ToString(TaskCancellationReason reason) noexcept {
    switch (reason) {
        case TaskCancellationReason::kNone:
            return "Not cancelled";
        case TaskCancellationReason::kUserRequest:
            return "User request";
        case TaskCancellationReason::kDeadline:
            return "Task deadline reached";
        case TaskCancellationReason::kOverload:
            return "Task processor overload";
        case TaskCancellationReason::kOOM:
            return "Not enough memory";
        case TaskCancellationReason::kAbandoned:
            return "Task destructor is called before the payload finished execution";
        case TaskCancellationReason::kShutdown:
            return "Task processor shutdown";
    }

    utils::AbortWithStacktrace(fmt::format("Garbage task cancellation reason: {}", utils::UnderlyingValue(reason)));
}

TaskCancellationToken::TaskCancellationToken() noexcept = default;

TaskCancellationToken::TaskCancellationToken(impl::TaskContext& context) noexcept : context_(&context) {}

TaskCancellationToken::TaskCancellationToken(Task& task) : context_(task.pimpl_->context) { UASSERT(context_); }

// clang-tidy insists on defaulting this,
// gcc complains about exception-specification mismatch with '= default'
// NOLINTNEXTLINE(hicpp-use-equals-default,modernize-use-equals-default)
TaskCancellationToken::TaskCancellationToken(const TaskCancellationToken& other) noexcept : context_{other.context_} {}

TaskCancellationToken::TaskCancellationToken(TaskCancellationToken&&) noexcept = default;

TaskCancellationToken& TaskCancellationToken::operator=(const TaskCancellationToken& other) noexcept {
    if (&other != this) {
        context_ = other.context_;
    }

    return *this;
}

TaskCancellationToken& TaskCancellationToken::operator=(TaskCancellationToken&&) noexcept = default;

TaskCancellationToken::~TaskCancellationToken() = default;

void TaskCancellationToken::RequestCancel() {
    UASSERT(context_);
    context_->RequestCancel(TaskCancellationReason::kUserRequest);
}

TaskCancellationReason TaskCancellationToken::CancellationReason() const noexcept {
    UASSERT(context_);
    return context_->CancellationReason();
}

bool TaskCancellationToken::IsCancelRequested() const noexcept {
    UASSERT(context_);
    return context_->IsCancelRequested();
}

bool TaskCancellationToken::IsValid() const noexcept { return !!context_; }

}  // namespace engine

USERVER_NAMESPACE_END
