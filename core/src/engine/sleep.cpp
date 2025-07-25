#include <userver/engine/sleep.hpp>

#include <userver/engine/task/cancel.hpp>

#include <engine/task/task_context.hpp>
#include <userver/utils/fast_scope_guard.hpp>

USERVER_NAMESPACE_BEGIN

namespace engine {

namespace impl {
namespace {
class CommonSleepWaitStrategy final : public WaitStrategy {
public:
    CommonSleepWaitStrategy() = default;

    EarlyWakeup SetupWakeups() override { return EarlyWakeup{false}; }

    void DisableWakeups() noexcept override {}
};
}  // namespace
}  // namespace impl

void InterruptibleSleepUntil(Deadline deadline) {
    auto& current = current_task::GetCurrentTaskContext();
    const utils::FastScopeGuard reset_background([&current, previous_background_flag = current.IsBackground()](
                                                 ) noexcept { current.SetBackground(previous_background_flag); });
    current.SetBackground(true);
    impl::CommonSleepWaitStrategy wait_manager{};
    current.Sleep(wait_manager, deadline);
}

void SleepUntil(Deadline deadline) {
    const TaskCancellationBlocker block_cancel;
    InterruptibleSleepUntil(deadline);
}

void Yield() { SleepUntil(Deadline::Passed()); }

}  // namespace engine

USERVER_NAMESPACE_END
