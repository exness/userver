#include <userver/components/manager_controller_component.hpp>

#include <components/manager.hpp>
#include <engine/task/task_processor.hpp>
#include <engine/task/task_processor_pools.hpp>
#include <userver/components/statistics_storage.hpp>
#include <userver/dynamic_config/storage/component.hpp>
#include <userver/dynamic_config/value.hpp>
#include <userver/logging/component.hpp>
#include <userver/utils/algo.hpp>

#include <dynamic_config/variables/USERVER_TASK_PROCESSOR_QOS.hpp>

USERVER_NAMESPACE_BEGIN

namespace {

void WriteRateAndLegacyMetrics(utils::statistics::Writer&& writer, utils::statistics::Rate metric) {
    writer = metric.value;
    writer["v2"] = metric;
}

}  // namespace

namespace engine {

void DumpMetric(utils::statistics::Writer& writer, const engine::TaskProcessor& task_processor) {
    const auto& counter = task_processor.GetTaskCounter();

    const auto destroyed = counter.GetDestroyedTasks();
    const auto created = counter.GetCreatedTasks();
    const auto stopped = counter.GetStoppedTasks();

    if (auto tasks = writer["tasks"]) {
        WriteRateAndLegacyMetrics(tasks["created"], created);
        tasks["alive"] = (created - std::min(destroyed, created)).value;
        tasks["running"] = counter.GetRunningTasks();
        tasks["queued"] = GetQueueSize(task_processor);
        WriteRateAndLegacyMetrics(tasks["finished"], stopped);
        WriteRateAndLegacyMetrics(tasks["cancelled"], counter.GetCancelledTasks());
        WriteRateAndLegacyMetrics(tasks["cancelled_overload"], counter.GetCancelledTasksOverload());
    }

    writer["errors"].ValueWithLabels(counter.GetTasksOverload(), {{"task_processor_error", "wait_queue_overload"}});

    if (auto context_switch = writer["context_switch"]) {
        WriteRateAndLegacyMetrics(context_switch["slow"], counter.GetTasksStartedRunning());
        WriteRateAndLegacyMetrics(context_switch["spurious_wakeups"], counter.GetSpuriousWakeups());

        WriteRateAndLegacyMetrics(context_switch["overloaded"], counter.GetTasksOverloadSensor());
        WriteRateAndLegacyMetrics(context_switch["no_overloaded"], counter.GetTasksNoOverloadSensor());
    }

    writer["worker-threads"] = task_processor.GetWorkerCount();
}

}  // namespace engine

namespace components {

ManagerControllerComponent::ManagerControllerComponent(
    const components::ComponentConfig&,
    const components::ComponentContext& context
)
    : components_manager_(context.GetManager(utils::impl::InternalTag{})) {
    auto& storage = context.FindComponent<components::StatisticsStorage>().GetStorage();

    auto config_source = context.FindComponent<DynamicConfig>().GetSource();
    config_subscription_ =
        config_source.UpdateAndListen(this, "engine_controller", &ManagerControllerComponent::OnConfigUpdate);

    statistics_holder_ =
        storage.RegisterWriter("engine", [this](utils::statistics::Writer& writer) { return WriteStatistics(writer); });

    auto& logger_component = context.FindComponent<components::Logging>();
    for (const auto& [name, task_processor] : components_manager_.GetTaskProcessorsMap()) {
        const auto& logger_name = task_processor->GetTaskTraceLoggerName();
        if (!logger_name.empty()) {
            auto logger = logger_component.GetLogger(logger_name);
            task_processor->SetTaskTraceLogger(std::move(logger));
        }
    }
}

ManagerControllerComponent::~ManagerControllerComponent() {
    statistics_holder_.Unregister();
    config_subscription_.Unsubscribe();
}

void ManagerControllerComponent::WriteStatistics(utils::statistics::Writer& writer) {
    // task processors
    for (const auto& [name, task_processor] : components_manager_.GetTaskProcessorsMap()) {
        writer["task-processors"].ValueWithLabels(*task_processor, {{"task_processor", name}});
    }

    // ev-threads
    const auto& pools_ptr = components_manager_.GetTaskProcessorPools();
    writer["ev-threads"]["cpu-load-percent"] = pools_ptr->EventThreadPool();

    // coroutines
    if (auto coro_pool = writer["coro-pool"]) {
        const auto stats = components_manager_.GetTaskProcessorPools()->GetCoroPool().GetStats();
        if (auto coro_stats = coro_pool["coroutines"]) {
            coro_stats["active"] = stats.active_coroutines;
            coro_stats["total"] = stats.total_coroutines;
        }
        if (auto stack_usage_stats = coro_pool["stack-usage"]) {
            stack_usage_stats["max-usage-percent"] = stats.max_stack_usage_pct;
            stack_usage_stats["is-monitor-active"] = stats.is_stack_usage_monitor_active;
        }
    }

    // misc
    writer["uptime-seconds"] = std::chrono::duration_cast<std::chrono::seconds>(
                                   std::chrono::steady_clock::now() - components_manager_.GetStartTime()
    )
                                   .count();
    writer["load-ms"] =
        std::chrono::duration_cast<std::chrono::milliseconds>(components_manager_.GetLoadDuration()).count();
}

void ManagerControllerComponent::OnConfigUpdate(const dynamic_config::Snapshot& cfg) {
    auto config = cfg[::dynamic_config::USERVER_TASK_PROCESSOR_QOS];
    const auto& profiler_config = cfg[::dynamic_config::USERVER_TASK_PROCESSOR_PROFILER_DEBUG];

    for (const auto& [name, task_processor] : components_manager_.GetTaskProcessorsMap()) {
        auto profiler_cfg = utils::FindOrDefault(
            profiler_config.extra, name, utils::FindOrDefault(profiler_config.extra, "default-task-processor")
        );
        // NOTE: find task processor by name, someday
        task_processor->SetSettings(config.default_service.default_task_processor, profiler_cfg);
    }
}

}  // namespace components

USERVER_NAMESPACE_END
