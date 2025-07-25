#include "task_context.hpp"

#include <exception>
#include <utility>

#include <fmt/format.h>
#include <boost/exception/diagnostic_information.hpp>

#include <engine/coro/pool.hpp>
#include <engine/coro/stack_usage_monitor.hpp>
#include <logging/log_extra_stacktrace.hpp>
#include <userver/compiler/impl/tls.hpp>
#include <userver/compiler/impl/tsan.hpp>
#include <userver/compiler/thread_local.hpp>
#include <userver/engine/exception.hpp>
#include <userver/engine/impl/task_context_factory.hpp>
#include <userver/engine/task/cancel.hpp>
#include <userver/logging/log.hpp>
#include <userver/logging/stacktrace_cache.hpp>
#include <userver/utils/assert.hpp>
#include <userver/utils/underlying_value.hpp>

#include <engine/ev/thread_pool.hpp>
#include <engine/impl/future_utils.hpp>
#include <engine/impl/generic_wait_list.hpp>
#include <engine/task/coro_unwinder.hpp>
#include <engine/task/cxxabi_eh_globals.hpp>
#include <engine/task/task_processor.hpp>

#include <gdb_autogen/cmd/utask/cmd.hpp>

USERVER_NAMESPACE_BEGIN

namespace engine {

namespace current_task {
namespace {

compiler::ThreadLocal current_task_context_ptr = []() -> engine::impl::TaskContext* { return nullptr; };

void SetCurrentTaskContext(engine::impl::TaskContext* context) {
    auto local_task_context_ptr = current_task_context_ptr.Use();
    UASSERT(!*local_task_context_ptr || !context);
    *local_task_context_ptr = context;
}

}  // namespace

engine::impl::TaskContext& GetCurrentTaskContext() noexcept {
    auto current_task_context = current_task_context_ptr.Use();
    if (!*current_task_context) {
        // AbortWithStacktrace MUST be a separate function! Putting the body of this
        // function into GetCurrentTaskContext() clobbers too many registers and
        // compiler decides to use stack memory in GetCurrentTaskContext(). This
        // leads to slowdown of GetCurrentTaskContext(). In particular Mutex::lock()
        // slows down on ~25%.
        utils::AbortWithStacktrace(
            "current_task::GetCurrentTaskContext() has been called "
            "outside of coroutine context"
        );
    }
    return **current_task_context;
}

engine::impl::TaskContext* GetCurrentTaskContextUnchecked() noexcept {
    auto current_task_context = current_task_context_ptr.Use();
    return *current_task_context;
}

}  // namespace current_task

namespace impl {

[[noreturn]] void ReportDeadlock() { UINVARIANT(false, "Coroutine attempted to wait for itself"); }

namespace {

auto ReadableTaskId(const TaskContext* task) noexcept { return logging::HexShort(task ? task->GetTaskId() : 0); }

class CurrentTaskScope final {
public:
    explicit CurrentTaskScope(TaskContext& context, EhGlobals& eh_store) : eh_store_(eh_store) {
        current_task::SetCurrentTaskContext(&context);
        ExchangeEhGlobals(eh_store_);
    }

    ~CurrentTaskScope() {
        ExchangeEhGlobals(eh_store_);
        current_task::SetCurrentTaskContext(nullptr);
    }

private:
    EhGlobals& eh_store_;
};

constexpr SleepState MakeNextEpochSleepState(SleepState::Epoch current) {
    using Epoch = SleepState::Epoch;
    return {SleepFlags::kNone, Epoch{utils::UnderlyingValue(current) + 1}};
}

auto* const kFinishedDetachedToken = reinterpret_cast<DetachedTasksSyncBlock::Token*>(1);

}  // namespace

TaskContext::TaskContext(
    TaskProcessor& task_processor,
    Task::Importance importance,
    Task::WaitMode wait_type,
    Deadline deadline,
    utils::impl::WrappedCallBase& payload
)
    : task_processor_(task_processor),
      task_counter_token_(task_processor_.GetTaskCounter()),
      is_critical_(importance == Task::Importance::kCritical),
      payload_(&payload),
      finish_waiters_(wait_type),
      cancel_deadline_(deadline),
      trace_csw_left_(task_processor_.GetTaskTraceMaxCswForNewTask()) {
    UASSERT(payload_);
    LOG_TRACE() << "task with task_id=" << ReadableTaskId(current_task::GetCurrentTaskContextUnchecked())
                << " created task with task_id=" << ReadableTaskId(this) << logging::LogExtra::Stacktrace();

    TsanReleaseBarrier();
}

TaskContext::~TaskContext() noexcept {
    LOG_TRACE() << "Task with task_id=" << ReadableTaskId(this) << " stopped" << logging::LogExtra::Stacktrace();
    UASSERT(magic_ == kMagic);

    UASSERT(state_ == Task::State::kNew || IsFinished());
    UASSERT(detached_token_ == nullptr || detached_token_ == kFinishedDetachedToken);

    UASSERT(payload_ == nullptr);
}

utils::impl::WrappedCallBase& TaskContext::GetPayload() noexcept {
    UASSERT(state_.load(std::memory_order_relaxed) == Task::State::kCompleted);
    UASSERT(payload_);
    return *payload_;
}

bool TaskContext::IsCurrent() const noexcept { return this == current_task::GetCurrentTaskContextUnchecked(); }

bool TaskContext::IsCritical() const {
    // running tasks must not be susceptible to overload
    // e.g. we might need to run coroutine to cancel it
    return WasStartedAsCritical() || coro_;
}

bool TaskContext::IsSharedWaitAllowed() const { return finish_waiters_->IsShared(); }

bool TaskContext::IsFinished() const noexcept { return finish_waiters_->IsSignaled(); }

void TaskContext::SetDetached(DetachedTasksSyncBlock::Token& token) noexcept {
    DetachedTasksSyncBlock::Token* expected = nullptr;
    if (!detached_token_.compare_exchange_strong(expected, &token)) {
        UASSERT(expected == kFinishedDetachedToken);
        DetachedTasksSyncBlock::Dispose(token);
    }
}

void TaskContext::FinishDetached() noexcept {
    auto* const token = detached_token_.exchange(kFinishedDetachedToken);
    if (token != nullptr && token != kFinishedDetachedToken) {
        DetachedTasksSyncBlock::Dispose(*token);
    }
}

FutureStatus TaskContext::WaitUntil(Deadline deadline) const noexcept {
    // try to avoid ctx switch if possible
    static_assert(noexcept(IsFinished()));
    if (IsFinished()) return FutureStatus::kReady;

    static_assert(noexcept(current_task::GetCurrentTaskContext()));
    auto& current = current_task::GetCurrentTaskContext();

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    auto& target = const_cast<TaskContext&>(*this);

    static_assert(noexcept(FutureWaitStrategy{target, current}));
    auto wait_strategy = FutureWaitStrategy{target, current};

    try {
        const auto wakeup_source = current.Sleep(wait_strategy, deadline);
        return ToFutureStatus(wakeup_source);
    } catch (...) {
        // We cannot just refuse to wait because of the lifetime guarantees for tasks and their data.
        utils::AbortWithStacktrace(
            "Unexpected exception from Sleep: " + boost::current_exception_diagnostic_information()
        );
    }
}

void TaskContext::DoStep() {
    if (IsFinished()) return;

    SleepState::Flags clear_flags{SleepFlags::kSleeping};
    if (!coro_) {
        try {
            coro_ = task_processor_.GetCoroutine();
        } catch (...) {
            // Seems we're out of memory
            cancellation_reason_ = TaskCancellationReason::kOOM;
            SetState(TaskBase::State::kCancelled);
            finish_waiters_->SetSignalAndWakeupAll();
            throw;
        }

        clear_flags |= SleepFlags::kWakeupByBootstrap;
        ArmCancellationTimer();
    }
    sleep_state_.ClearFlags<std::memory_order_relaxed>(clear_flags);

    // eh_globals is replaced in task scope, we must proxy the exception
    std::exception_ptr uncaught;
    {
        const CurrentTaskScope current_task_scope(*this, eh_globals_);
        try {
            SetState(Task::State::kRunning);
            auto& coro_ref = *coro_;
            TsanAcquireBarrier();
            coro_ref(this);
        } catch (...) {
            uncaught = std::current_exception();
        }
        TsanReleaseBarrier();
    }

    if (uncaught) std::rethrow_exception(uncaught);

    switch (yield_reason_) {
        case YieldReason::kTaskCancelled:
        case YieldReason::kTaskComplete: {
            std::move(coro_).ReturnToPool();
            auto new_state =
                (yield_reason_ == YieldReason::kTaskComplete) ? Task::State::kCompleted : Task::State::kCancelled;
            if (cancellation_reason_.load(std::memory_order_relaxed) != TaskCancellationReason::kNone) {
                GetTaskProcessor().GetTaskCounter().AccountTaskCancel();
            }
            SetState(new_state);
            deadline_timer_.Finalize();
            finish_waiters_->SetSignalAndWakeupAll();
            TraceStateTransition(new_state);
        } break;

        case YieldReason::kTaskWaiting:
            SetState(Task::State::kSuspended);
            {
                SleepState::Flags new_flags = SleepFlags::kSleeping;
                if (!IsCancellable()) new_flags |= SleepFlags::kNonCancellable;

                // Synchronization point for relaxed SetState()
                auto prev_sleep_state = sleep_state_.FetchOrFlags<std::memory_order_seq_cst>(new_flags);

                // The previous kWakeupBy* flags in sleep_state_ are not cleared here,
                // which allows RequestCancel to cancel the next sleep session.
                UASSERT(!(prev_sleep_state.flags & SleepFlags::kSleeping));
                if (new_flags & SleepFlags::kNonCancellable)
                    prev_sleep_state.flags.Clear({SleepFlags::kWakeupByCancelRequest, SleepFlags::kNonCancellable});
                if (prev_sleep_state.flags) {
                    Schedule();
                }
            }
            break;

        case YieldReason::kNone:
            UINVARIANT(false, "invalid yield reason");
    }
}

void TaskContext::RequestCancel(TaskCancellationReason reason) {
    auto expected = TaskCancellationReason::kNone;
    if (cancellation_reason_.compare_exchange_strong(expected, reason)) {
        LOG_TRACE() << "task with task_id=" << ReadableTaskId(current_task::GetCurrentTaskContextUnchecked())
                    << " cancelled task with task_id=" << ReadableTaskId(this) << logging::LogExtra::Stacktrace();
        const auto epoch = GetEpoch();
        Wakeup(WakeupSource::kCancelRequest, epoch);
    }
}

bool TaskContext::IsCancellable() const noexcept { return is_cancellable_; }

bool TaskContext::SetCancellable(bool value) {
    UASSERT(IsCurrent());
    UASSERT(state_ == Task::State::kRunning);

    return std::exchange(is_cancellable_, value);
}

void TaskContext::SetBackground(bool is_background) {
    UASSERT(IsCurrent());
    UASSERT(state_ == Task::State::kRunning);
    is_background_ = is_background;
}

TaskContext::WakeupSource TaskContext::Sleep(WaitStrategy& wait_strategy, Deadline deadline) {
    UASSERT(IsCurrent());
    UASSERT(state_ == Task::State::kRunning);
    UASSERT_MSG(
        compiler::impl::AreCoroutineSwitchesAllowed(),
        "Coroutine context switches are forbidden in the current scope, "
        "which is likely working with thread-local variables"
    );

    UASSERT_MSG(!std::exchange(within_sleep_, true), "Recursion in Sleep detected");
    const utils::FastScopeGuard guard{[this]() noexcept {
        UASSERT_MSG(std::exchange(within_sleep_, false), "within_sleep_ should report being in Sleep");
    }};

    // If the previous Sleep woke up due to both kCancelRequest and kWaitList, the
    // cancellation signal would be lost, so we must check it here.
    if (ShouldCancel()) return TaskContext::WakeupSource::kCancelRequest;

    const auto sleep_epoch = sleep_state_.Load<std::memory_order_seq_cst>().epoch;

    if (static_cast<bool>(wait_strategy.SetupWakeups())) {
        sleep_state_.Store<std::memory_order_release>(MakeNextEpochSleepState(sleep_epoch));
        wakeup_source_ = WakeupSource::kWaitList;
        return wakeup_source_;
    }

    const bool has_deadline = deadline.IsReachable() && (!IsCancellable() || deadline < cancel_deadline_);
    if (has_deadline) ArmDeadlineTimer(deadline, sleep_epoch);

    yield_reason_ = YieldReason::kTaskWaiting;
    UASSERT(task_pipe_);
    TraceStateTransition(Task::State::kSuspended);
    ProfilerStopExecution();

    auto& task_pipe_ref = *task_pipe_;
    TsanAcquireBarrier();
    [[maybe_unused]] TaskContext* context = task_pipe_ref().get();
    TsanReleaseBarrier();

    ProfilerStartExecution();
    TraceStateTransition(Task::State::kRunning);
    UASSERT(context == this);
    UASSERT(state_ == Task::State::kRunning);

    if (has_deadline) ArmCancellationTimer();
    wait_strategy.DisableWakeups();

    const auto old_sleep_state = sleep_state_.Exchange<std::memory_order_acq_rel>(MakeNextEpochSleepState(sleep_epoch));
    wakeup_source_ = GetPrimaryWakeupSource(old_sleep_state.flags);
    return wakeup_source_;
}

void TaskContext::ArmDeadlineTimer(Deadline deadline, SleepState::Epoch sleep_epoch) {
    UASSERT(deadline.IsReachable());
    if (deadline_timer_.WasStarted()) {
        deadline_timer_.RestartWakeup(deadline, sleep_epoch);
    } else {
        deadline_timer_.StartWakeup(
            boost::intrusive_ptr{this}, task_processor_.EventThreadPool().NextTimerThread(), deadline, sleep_epoch
        );
    }
}

void TaskContext::ArmCancellationTimer() {
    if (!cancel_deadline_.IsReachable()) {
        return;
    }

    if (deadline_timer_.WasStarted()) {
        deadline_timer_.RestartCancel(cancel_deadline_);
    } else {
        deadline_timer_.StartCancel(
            boost::intrusive_ptr{this}, task_processor_.EventThreadPool().NextTimerThread(), cancel_deadline_
        );
    }
}

bool TaskContext::ShouldSchedule(SleepState::Flags prev_flags, WakeupSource source) {
    /* ShouldSchedule() returns true only for the first Wakeup().  All Wakeup()s
     * are serialized due to seq_cst in FetchOr().
     */

    if (!(prev_flags & SleepFlags::kSleeping)) return false;

    if (source == WakeupSource::kCancelRequest) {
        /* Don't wakeup if:
         * 1) kNonCancellable
         * 2) Other WakeupSource is already triggered
         */
        return prev_flags == SleepFlags::kSleeping;
    } else if (source == WakeupSource::kBootstrap) {
        return true;
    } else {
        if (prev_flags & SleepFlags::kNonCancellable) {
            /* If there was a cancellation request, but cancellation is blocked,
             * ignore it - we're the first to Schedule().
             */
            prev_flags.Clear({SleepFlags::kNonCancellable, SleepFlags::kWakeupByCancelRequest});
        }

        /* Don't wakeup if:
         * 1) kNonCancellable and zero or more kCancelRequest triggered
         * 2) !kNonCancellable and any other WakeupSource is triggered
         */

        // We're the first to wakeup the baby
        return prev_flags == SleepFlags::kSleeping;
    }
}

SleepState::Epoch TaskContext::GetEpoch() noexcept { return sleep_state_.Load<std::memory_order_acquire>().epoch; }

void TaskContext::Wakeup(WakeupSource source, SleepState::Epoch epoch) {
    if (IsFinished()) return;

    auto prev_sleep_state = sleep_state_.Load<std::memory_order_relaxed>();

    while (true) {
        if (prev_sleep_state.epoch != epoch) {
            // Epoch changed, wakeup is for some previous sleep
            return;
        }

        if (source == WakeupSource::kCancelRequest && prev_sleep_state.flags & SleepFlags::kNonCancellable) {
            // We do not need to wakeup because:
            // - *this is non cancellable and the epoch is correct
            // - or even if the sleep_state_ changed and the task is now cancellable
            //   then epoch changed and wakeup request is not for the current sleep.
            return;
        }

        auto new_sleep_state = prev_sleep_state;
        new_sleep_state.flags |= static_cast<SleepFlags>(source);
        if (sleep_state_.CompareExchangeWeak<std::memory_order_relaxed, std::memory_order_relaxed>(
                prev_sleep_state, new_sleep_state
            )) {
            break;
        }
    }

    if (ShouldSchedule(prev_sleep_state.flags, source)) {
        Schedule();
    }
}

void TaskContext::Wakeup(WakeupSource source, NoEpoch) {
    UASSERT(source != WakeupSource::kDeadlineTimer);
    UASSERT(source != WakeupSource::kBootstrap);
    UASSERT(source != WakeupSource::kCancelRequest);

    if (IsFinished()) return;

    // Set flag regardless of kSleeping - missing kSleeping usually means one of
    // the following: 1) the task is somewhere between Sleep() and setting
    // kSleeping in DoStep(). 2) the task is already awaken, but DisableWakeups()
    // is not yet finished (and not all timers/watchers are stopped).
    const auto prev_sleep_state = sleep_state_.FetchOrFlags<std::memory_order_seq_cst>(static_cast<SleepFlags>(source));
    if (ShouldSchedule(prev_sleep_state.flags, source)) {
        Schedule();
    }
}

class TaskContext::YieldReasonGuard {
public:
    explicit YieldReasonGuard(TaskContext& context) noexcept : context_(context) {}

    ~YieldReasonGuard() noexcept { context_.yield_reason_ = yield_reason_; }

    void SetYieldReason(YieldReason reason) noexcept { yield_reason_ = reason; }

private:
    TaskContext& context_;
    YieldReason yield_reason_{YieldReason::kNone};
};

class TaskContext::LocalStorageGuard {
public:
    explicit LocalStorageGuard(TaskContext& context) : context_(context) { context_.local_storage_.emplace(); }

    ~LocalStorageGuard() { context_.local_storage_.reset(); }

private:
    TaskContext& context_;
};

class TaskContext::ProfilerExecutionGuard {
public:
    explicit ProfilerExecutionGuard(TaskContext& context) noexcept : context_(context) {
        static_assert(noexcept(context_.ProfilerStartExecution()));
        context_.ProfilerStartExecution();
    }

    ~ProfilerExecutionGuard() {
        static_assert(noexcept(context_.ProfilerStopExecution()));
        context_.ProfilerStopExecution();
    }

private:
    TaskContext& context_;
};

void TaskContext::CoroFunc(TaskPipe& task_pipe) {
    for (TaskContext* context : task_pipe) {
        UASSERT(context);
        context->TsanReleaseBarrier();
        context->task_pipe_ = &task_pipe;

        {
            // Set yield_reason_ outside ~LocalStorageGuard as Sleep in dtors would otherwise clobber it.
            YieldReasonGuard yield_reason_guard(*context);
            // Destroy contents of LocalStorage in the coroutine as dtors may want to schedule.
            const LocalStorageGuard local_storage_guard(*context);
            // Uses task-local storage for logging.
            const ProfilerExecutionGuard profiler_execution_guard(*context);

            // We only let tasks ran with CriticalAsync enter function body, others
            // get terminated ASAP.
            if (context->IsCancelRequested() && !context->WasStartedAsCritical()) {
                context->SetCancellable(false);
                // It is important to destroy payload here as someone may want
                // to synchronize in its dtor (e.g. lambda closure).
                context->ResetPayload();
                yield_reason_guard.SetYieldReason(YieldReason::kTaskCancelled);
            } else {
                try {
                    context->TraceStateTransition(Task::State::kRunning);
                    context->payload_->Perform();
                    yield_reason_guard.SetYieldReason(YieldReason::kTaskComplete);
                } catch (const CoroUnwinder&) {
                    yield_reason_guard.SetYieldReason(YieldReason::kTaskCancelled);
                } catch (...) {
                    utils::AbortWithStacktrace(
                        "An exception that is not derived from std::exception has been "
                        "thrown: " +
                        boost::current_exception_diagnostic_information() +
                        " Such exceptions are not supported by userver."
                    );
                }
            }
        }

        context->task_pipe_ = nullptr;
        context->TsanAcquireBarrier();
    }
}

void TaskContext::SetCancelDeadline(Deadline deadline) {
    UASSERT(IsCurrent());
    UASSERT(state_ == Task::State::kRunning);
    cancel_deadline_ = deadline;
    ArmCancellationTimer();
}

bool TaskContext::HasLocalStorage() const noexcept { return local_storage_.has_value(); }

task_local::Storage& TaskContext::GetLocalStorage() noexcept {
    UASSERT(local_storage_);
    return *local_storage_;
}

bool TaskContext::IsReady() const noexcept { return IsFinished(); }

EarlyWakeup TaskContext::TryAppendWaiter(TaskContext& waiter) {
    if (&waiter == this) ReportDeadlock();
    return EarlyWakeup{finish_waiters_->GetSignalOrAppend(&waiter)};
}

void TaskContext::RemoveWaiter(TaskContext& waiter) noexcept { finish_waiters_->Remove(waiter); }

void TaskContext::AfterWait() noexcept {}

void TaskContext::RethrowErrorResult() const {
    UASSERT(IsFinished());
    if (state_.load(std::memory_order_relaxed) != Task::State::kCompleted) {
        throw TaskCancelledException(CancellationReason());
    }
    payload_->RethrowErrorResult();
}

size_t TaskContext::UseCount() const noexcept {
    // memory order could potentially be less restrictive, but it gets very
    // complicated to reason about
    return intrusive_refcount_.load(std::memory_order_seq_cst);
}

std::size_t TaskContext::DecrementFetchSharedTaskUsages() noexcept { return --shared_task_usages_; }

std::size_t TaskContext::IncrementFetchSharedTaskUsages() noexcept { return ++shared_task_usages_; }

TaskContext::WakeupSource TaskContext::GetPrimaryWakeupSource(SleepState::Flags sleep_flags) {
    static constexpr std::pair<SleepState::Flags, WakeupSource> l[] = {
        {SleepFlags::kWakeupByWaitList, WakeupSource::kWaitList},
        {SleepFlags::kWakeupByDeadlineTimer, WakeupSource::kDeadlineTimer},
        {SleepFlags::kWakeupByBootstrap, WakeupSource::kBootstrap},
    };
    for (auto it : l)
        if (sleep_flags & it.first) return it.second;

    if ((sleep_flags & SleepFlags::kWakeupByCancelRequest) && !(sleep_flags & SleepFlags::kNonCancellable))
        return WakeupSource::kCancelRequest;

    UINVARIANT(false, fmt::format("Cannot find valid wakeup source for {}", sleep_flags.GetValue()));
}

bool TaskContext::WasStartedAsCritical() const { return is_critical_; }

void TaskContext::SetState(Task::State new_state) {
    // 'release', because if someone detects kCompleted or kCancelled by running
    // in a loop, they should acquire the task's results.
    state_.store(new_state, std::memory_order_release);
}

void TaskContext::Schedule() {
    UASSERT(state_ != Task::State::kQueued);
    SetState(Task::State::kQueued);
    TraceStateTransition(Task::State::kQueued);
    task_processor_.Schedule(this);
    // NOTE: may be executed at this point
}

void TaskContext::ProfilerStartExecution() noexcept {
    auto threshold_us = task_processor_.GetProfilerThreshold();
    if (threshold_us.count() > 0) {
        execute_started_ = std::chrono::steady_clock::now();
    } else {
        execute_started_ = {};
    }
}

void TaskContext::ProfilerStopExecution() noexcept {
    auto threshold_us = task_processor_.GetProfilerThreshold();
    if (threshold_us.count() <= 0) return;

    if (execute_started_ == std::chrono::steady_clock::time_point{}) {
        // the task was started w/o profiling, skip it
        return;
    }

    auto now = std::chrono::steady_clock::now();
    auto duration = now - execute_started_;
    auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(duration);

    if (duration_us >= threshold_us) {
        logging::LogExtra extra_stacktrace;
        if (task_processor_.ShouldProfilerForceStacktrace()) {
            logging::impl::ExtendLogExtraWithStacktrace(extra_stacktrace);
        }
        LOG_ERROR() << "Profiler threshold reached, task was executing "
                       "for too long without context switch ("
                    << duration_us.count() << "us >= " << threshold_us.count() << "us)" << extra_stacktrace;
    }
}

void TaskContext::TraceStateTransition(Task::State state) {
    if (trace_csw_left_ == 0) return;
    --trace_csw_left_;

    auto now = std::chrono::steady_clock::now();
    auto diff = now - last_state_change_timepoint_;
    if (last_state_change_timepoint_ == std::chrono::steady_clock::time_point()) diff = {};
    auto diff_us = std::chrono::duration_cast<std::chrono::microseconds>(diff).count();
    last_state_change_timepoint_ = now;

    auto logger = task_processor_.GetTaskTraceLogger();
    if (!logger) {
        return;
    }

    LOG_INFO_TO(*logger) << "Task " << logging::HexShort(GetTaskId()) << " changed state to "
                         << Task::GetStateName(state) << ", delay = " << diff_us << "us"
                         << logging::LogExtra::Stacktrace(*logger);
}

void TaskContext::ResetPayload() noexcept {
    if (!payload_) return;

    std::destroy_at(std::exchange(payload_, nullptr));
}

CountedCoroutinePtr& TaskContext::GetCoroutinePtr() noexcept { return coro_; }

void intrusive_ptr_add_ref(TaskContext* p) noexcept {
    UASSERT(p);

    // memory order could potentially be less restrictive, but it gets very
    // complicated to reason about
    p->intrusive_refcount_.fetch_add(1, std::memory_order_seq_cst);
}

void intrusive_ptr_release(TaskContext* p) noexcept {
    UASSERT(p);

    // memory order could potentially be less restrictive, but it gets very
    // complicated to reason about
    if (p->intrusive_refcount_.fetch_sub(1, std::memory_order_seq_cst) == 1) {
        p->ResetPayload();

        std::destroy_at(p);

        DeleteFusedTaskContext(reinterpret_cast<std::byte*>(p));
    }
}

bool HasWaitSucceeded(TaskContext::WakeupSource wakeup_source) noexcept {
    // Typical synchronization primitives sleep in a WaitList until woken up
    // (which is counted as a success), or they can sometimes wake themselves up
    // using kWaitList.
    switch (wakeup_source) {
        case TaskContext::WakeupSource::kWaitList:
            return true;
        case TaskContext::WakeupSource::kDeadlineTimer:
        case TaskContext::WakeupSource::kCancelRequest:
            return false;
        case TaskContext::WakeupSource::kNone:
        case TaskContext::WakeupSource::kBootstrap:
            UASSERT(false);
    }

    // Assume that bugs with an unexpected WakeupSource don't reach production.
    return false;
}

void TaskContext::TsanAcquireBarrier() noexcept {
#if USERVER_IMPL_HAS_TSAN
    __tsan_acquire(this);
    __tsan_acquire(&coro_);
#endif
}

void TaskContext::TsanReleaseBarrier() noexcept {
#if USERVER_IMPL_HAS_TSAN
    __tsan_release(&coro_);
    __tsan_release(this);
#endif
}

}  // namespace impl
}  // namespace engine

#include "cxxabi_eh_globals.inc"

USERVER_NAMESPACE_END
