#include <engine/impl/standalone.hpp>

#include <future>

#include <engine/coro/pool_config.hpp>
#include <engine/ev/thread_pool_config.hpp>
#include <engine/task/task_processor.hpp>
#include <engine/task/task_processor_config.hpp>
#include <engine/task/task_processor_pools.hpp>
#include <userver/engine/async.hpp>

#include <userver/tracing/span.hpp>

USERVER_NAMESPACE_BEGIN

namespace engine::impl {

std::shared_ptr<TaskProcessorPools> MakeTaskProcessorPools(const TaskProcessorPoolsConfig& pools_config) {
    coro::PoolConfig coro_config;
    coro_config.initial_size = pools_config.initial_coro_pool_size;
    coro_config.max_size = pools_config.max_coro_pool_size;
    coro_config.stack_size = pools_config.coro_stack_size;
    coro_config.is_stack_usage_monitor_enabled = pools_config.is_stack_usage_monitor_enabled;

    ev::ThreadPoolConfig ev_config;
    ev_config.threads = pools_config.ev_threads_num;
    ev_config.thread_name = pools_config.ev_thread_name;
    ev_config.ev_default_loop_disabled = pools_config.ev_default_loop_disabled;

    return std::make_shared<TaskProcessorPools>(std::move(coro_config), std::move(ev_config));
}

TaskProcessorHolder
TaskProcessorHolder::Make(std::size_t threads_num, std::string thread_name, std::shared_ptr<TaskProcessorPools> pools) {
    TaskProcessorConfig config;
    config.worker_threads = threads_num;
    config.thread_name = std::move(thread_name);

    return TaskProcessorHolder(std::make_unique<TaskProcessor>(std::move(config), std::move(pools)));
}

TaskProcessorHolder::TaskProcessorHolder(std::unique_ptr<TaskProcessor>&& task_processor)
    : task_processor_(std::move(task_processor)) {}

TaskProcessorHolder::~TaskProcessorHolder() = default;

void RunOnTaskProcessorSync(TaskProcessor& tp, utils::function_ref<void()> user_cb) {
    UASSERT(!current_task::IsTaskProcessorThread());
    std::packaged_task<void()> packaged_task([&user_cb] {
        tracing::Span span("span");
        span.SetLogLevel(logging::Level::kNone);
        user_cb();
    });
    auto future = packaged_task.get_future();
    engine::DetachUnscopedUnsafe(engine::AsyncNoSpan(tp, std::move(packaged_task)));
    future.get();
}

}  // namespace engine::impl

USERVER_NAMESPACE_END
