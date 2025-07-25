#include <components/manager.hpp>

#include <chrono>
#include <future>
#include <set>
#include <stdexcept>
#include <thread>
#include <type_traits>

#include <fmt/core.h>
#include <fmt/ranges.h>
#include <boost/range/adaptor/map.hpp>
#include <boost/range/adaptor/transformed.hpp>

#include <components/component_context_impl.hpp>
#include <components/manager_config.hpp>
#include <engine/task/exception_hacks.hpp>
#include <engine/task/task_processor.hpp>
#include <engine/task/task_processor_pools.hpp>
#include <userver/components/component_list.hpp>
#include <userver/engine/async.hpp>
#include <userver/hostinfo/cpu_limit.hpp>
#include <userver/logging/component.hpp>
#include <userver/logging/log.hpp>
#include <userver/os_signals/component.hpp>
#include <userver/utils/algo.hpp>
#include <userver/utils/async.hpp>
#include <userver/utils/distances.hpp>

USERVER_NAMESPACE_BEGIN

namespace {

constexpr std::size_t kDefaultHwThreadsEstimate = 512;

template <typename Func>
auto RunInCoro(engine::TaskProcessor& task_processor, Func&& func) {
    UASSERT(!engine::current_task::IsTaskProcessorThread());
    auto task = engine::CriticalAsyncNoSpan(task_processor, std::forward<Func>(func));
    task.BlockingWait();
    return task.Get();
}

std::optional<size_t> GuessCpuLimit(const std::string& tp_name) {
    auto cpu_f = hostinfo::CpuLimit();
    if (!cpu_f) {
        return {};
    }

    const auto hw_concurrency = std::thread::hardware_concurrency();
    const auto hw_threads_estimate = hw_concurrency ? hw_concurrency : kDefaultHwThreadsEstimate;

    auto cpu = std::lround(*cpu_f);
    if (cpu > 0 && static_cast<unsigned int>(cpu) < hw_threads_estimate * 2) {
        // TODO: hack for https://st.yandex-team.ru/TAXICOMMON-2132
        if (cpu < 3) cpu = 3;

        LOG_INFO() << "Using CPU limit from env CPU_LIMIT (" << cpu << ") for worker_threads "
                   << "of task processor '" << tp_name << "', ignoring config value ";
        return cpu;
    }

    LOG_WARNING() << "CPU limit from env CPU_LIMIT (" << cpu_f
                  << ") looks very different from the estimated number of "
                     "hardware threads ("
                  << hw_threads_estimate << "), worker_threads from the static config will be used";
    return {};
}

void ValidateConfigs(
    const components::ComponentList& component_list,
    const components::ComponentConfigMap& component_config_map,
    components::ValidationMode validation_condition
) {
    std::vector<std::string> validation_errors;

    for (const auto& adder : component_list) {
        const auto it = component_config_map.find(adder->GetComponentName());
        UINVARIANT(
            it != component_config_map.cend(),
            fmt::format(
                "Component '{}' is registered , but not present in components_manager.components section of "
                "config.yaml.",
                adder->GetComponentName()
            )
        );
        try {
            adder->ValidateStaticConfig(it->second, validation_condition);
        } catch (const std::exception& exception) {
            auto component_name = adder->GetComponentName();
            validation_errors.push_back(fmt::format("{}: {}", component_name, exception.what()));
        }
    }

    if (!validation_errors.empty()) {
        throw std::runtime_error(fmt::format(
            "The following components have failed static config validation:\n\t{}", fmt::join(validation_errors, "\n\t")
        ));
    }
}

}  // namespace

namespace components::impl {

Manager::TaskProcessorsStorage::TaskProcessorsStorage(
    std::shared_ptr<engine::impl::TaskProcessorPools> task_processor_pools
)
    : task_processor_pools_(std::move(task_processor_pools)) {}

Manager::TaskProcessorsStorage::~TaskProcessorsStorage() {
    if (task_processor_pools_) Reset();
}

void Manager::TaskProcessorsStorage::Reset() noexcept {
    LOG_TRACE() << "Initiating task processors shutdown";
    for (auto& [name, task_processor] : task_processors_map_) {
        task_processor->InitiateShutdown();
    }
    LOG_TRACE() << "Waiting for all tasks to stop";
    WaitForAllTasksBlocking();
    LOG_TRACE() << "Stopping task processors";
    task_processors_map_.clear();
    LOG_TRACE() << "Stopped task processors";
    LOG_TRACE() << "Stopping task processor pools";
    UASSERT(task_processor_pools_.use_count() == 1);
    task_processor_pools_.reset();
    LOG_TRACE() << "Stopped task processor pools";
}

void Manager::TaskProcessorsStorage::Add(std::string name, std::unique_ptr<engine::TaskProcessor>&& task_processor) {
    task_processors_map_.emplace(std::move(name), std::move(task_processor));
}

void Manager::TaskProcessorsStorage::WaitForAllTasksBlocking() const noexcept {
    const auto indicators = task_processors_map_ | boost::adaptors::map_values |
                            boost::adaptors::transformed([](const auto& task_processor_ptr) -> const auto& {
                                const engine::TaskProcessor& task_processor = *task_processor_ptr;
                                return task_processor.GetTaskCounter();
                            });
    while (engine::impl::TaskCounter::AnyMayHaveTasksAlive(indicators)) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
}

Manager::Manager(std::unique_ptr<ManagerConfig>&& config, const ComponentList& component_list)
    : config_(std::move(config)),
      task_processors_storage_(
          std::make_shared<engine::impl::TaskProcessorPools>(config_->coro_pool, config_->event_thread_pool)
      ),
      start_time_(std::chrono::steady_clock::now()) {
    LOG_INFO() << "Starting components manager";

    for (auto processor_config : config_->task_processors) {
        if (processor_config.should_guess_cpu_limit) {
            if (config_->default_task_processor == processor_config.name) {
                auto guess_cpu = GuessCpuLimit(processor_config.name);
                if (guess_cpu) {
                    processor_config.worker_threads = *guess_cpu;
                }
            } else {
                LOG_ERROR() << "guess-cpu-limit is set for non-default task processor (" << processor_config.name
                            << "), ignoring it";
            }
        }
        task_processors_storage_.Add(
            processor_config.name,
            std::make_unique<engine::TaskProcessor>(processor_config, task_processors_storage_.GetTaskProcessorPools())
        );
    }
    const auto& task_processors_map = task_processors_storage_.GetMap();
    const auto default_task_processor_it = task_processors_map.find(config_->default_task_processor);
    if (default_task_processor_it == task_processors_map.end()) {
        throw std::runtime_error(
            "Cannot start components manager: failed to find default task processor with name '" +
            config_->default_task_processor + "'"
        );
    }

    UINVARIANT(!config_->fs_task_processor.empty(), "fs_task_processor cannot be empty");
    auto* fs_task_processor = utils::FindOrNullptr(task_processors_map, config_->fs_task_processor);
    UINVARIANT(
        fs_task_processor,
        utils::StrCat(
            "Cannot find task processor with name '", config_->fs_task_processor, "', is fs_task_processor correct?"
        )
    );

    for (auto& [name, tp] : task_processors_map) {
        tp->SetBlockingTaskProcessor(**fs_task_processor);
    }

    {
        // Call mlock() before component context creation as we should be done with
        // mlock before HTTP server starts and handles incoming requests
        const auto debug_info_action = config_->mlock_debug_info ? engine::impl::DebugInfoAction::kLockInMemory
                                                                 : engine::impl::DebugInfoAction::kLeaveAsIs;
        engine::impl::MLockDebugInfo(debug_info_action);
    }

    default_task_processor_ = default_task_processor_it->second.get();
    RunInCoro(*default_task_processor_, [this, &component_list]() { CreateComponentContext(component_list); });

    if (!config_->disable_phdr_cache) {
        engine::impl::InitPhdrCache();
    }

    LOG_INFO() << "Started components manager. All the components have started "
                  "successfully.";
}

Manager::~Manager() {
    LOG_INFO() << "Stopping components manager";

    try {
        RunInCoro(*default_task_processor_, [this] { component_context_->OnGracefulShutdownStarted(); });
    } catch (const std::exception& exc) {
        LOG_ERROR() << "Graceful shutdown failed: " << exc;
    }
    engine::impl::TeardownPhdrCacheAndEnableDynamicLoading();

    LOG_TRACE() << "Stopping component context";
    try {
        RunInCoro(*default_task_processor_, [this]() { ClearComponents(); });
    } catch (const std::exception& exc) {
        LOG_ERROR() << "Failed to clear components: " << exc;
    }
    component_context_.reset();
    LOG_TRACE() << "Stopped component context";
    task_processors_storage_.Reset();

    LOG_INFO() << "Stopped components manager";
}

const ManagerConfig& Manager::GetConfig() const { return *config_; }

const std::shared_ptr<engine::impl::TaskProcessorPools>& Manager::GetTaskProcessorPools() const {
    return task_processors_storage_.GetTaskProcessorPools();
}

const TaskProcessorsMap& Manager::GetTaskProcessorsMap() const { return task_processors_storage_.GetMap(); }

engine::TaskProcessor& Manager::GetTaskProcessor(std::string_view name) const {
    const auto& map = task_processors_storage_.GetMap();
    if (const auto* const task_processor = utils::impl::FindTransparentOrNullptr(map, name)) {
        return **task_processor;
    }
    throw std::runtime_error(fmt::format("Failed to find task processor with name: {}", name));
}

void Manager::OnSignal(int signum) {
    const std::shared_lock<std::shared_timed_mutex> lock(context_mutex_);
    if (components_cleared_) return;
    if (signal_processor_) {
        signal_processor_->Get().Notify(signum, utils::impl::InternalTag{});
    }
}

std::chrono::steady_clock::time_point Manager::GetStartTime() const { return start_time_; }

std::chrono::milliseconds Manager::GetLoadDuration() const { return load_duration_; }

void Manager::CreateComponentContext(const ComponentList& component_list) {
    std::set<std::string> loading_component_names;
    for (const auto& adder : component_list) {
        auto [it, inserted] = loading_component_names.insert(adder->GetComponentName());
        if (!inserted) {
            const std::string message = "duplicate component name in component_list: " + *it;
            LOG_ERROR() << message;
            throw std::runtime_error(message);
        }
    }

    for (const auto& component_config : config_->components) {
        const auto& name = component_config.Name();
        const auto it = loading_component_names.find(name);
        if (it == loading_component_names.cend()) {
            throw std::runtime_error(fmt::format(
                "Component with name '{}'"
                " found in static config, but no component with "
                "such name is registered in components::ComponentList in code.{}",
                name,
                utils::SuggestNearestName(loading_component_names, name)
            ));
        }

        // Delete component from context to make FindComponentOptional() work
        if (!component_config["load-enabled"].As<bool>(true)) {
            loading_component_names.erase(it);
        }
    }

    std::vector<std::string> loading_components;
    loading_components.reserve(loading_component_names.size());
    while (!loading_component_names.empty()) {
        auto node = loading_component_names.extract(loading_component_names.begin());
        loading_components.push_back(std::move(node.value()));
    }

    component_context_ = std::make_unique<impl::ComponentContextImpl>(*this, std::move(loading_components));

    AddComponents(component_list);
}

components::ComponentConfigMap Manager::MakeComponentConfigMap(const ComponentList& component_list) {
    components::ComponentConfigMap component_config_map;
    const auto component_count = std::distance(component_list.begin(), component_list.end());
    component_config_map.reserve(component_count);
    empty_configs_.reserve(component_count);

    for (const auto& component_config : config_->components) {
        const auto& name = component_config.Name();
        component_config_map.emplace(name, component_config);
    }

    for (const auto& item : component_list) {
        if (component_config_map.count(item->GetComponentName()) == 0 &&
            item->GetConfigFileMode() == ConfigFileMode::kNotRequired) {
            const auto& val = empty_configs_.emplace_back(item->GetComponentName());
            component_config_map.emplace(item->GetComponentName(), val);
        }
    }

    return component_config_map;
}

void Manager::AddComponents(const ComponentList& component_list) {
    const auto component_config_map = MakeComponentConfigMap(component_list);

    auto start_time = std::chrono::steady_clock::now();
    std::vector<engine::TaskWithResult<void>> tasks;
    bool is_load_cancelled = false;
    try {
        ValidateConfigs(component_list, component_config_map, config_->validate_components_configs);

        for (const auto& adder : component_list) {
            const auto& component_name = adder->GetComponentName();
            auto task_name = "boot/" + component_name;
            tasks.push_back(utils::CriticalAsync(std::move(task_name), [&]() {
                tracing::Span::CurrentSpan().AddTag("component_name", component_name);
                tracing::Span::CurrentSpan().SetLogLevel(logging::Level::kDebug);
                try {
                    AddComponentImpl(component_config_map, component_name, *adder);
                } catch (const ComponentsLoadCancelledException& ex) {
                    LOG_WARNING() << "Cannot start component " << component_name << ": " << ex;
                    component_context_->CancelComponentsLoad();
                    throw;
                } catch (const std::exception& ex) {
                    LOG_ERROR() << "Cannot start component " << component_name << ": " << ex;
                    component_context_->CancelComponentsLoad();
                    throw std::runtime_error(fmt::format("Cannot start component {}: {}", component_name, ex.what()));
                } catch (...) {
                    component_context_->CancelComponentsLoad();
                    throw;
                }
            }));
        }

        for (auto& task : tasks) {
            try {
                task.Get();
            } catch (const ComponentsLoadCancelledException&) {
                is_load_cancelled = true;
            }
        }
    } catch (const std::exception& ex) {
        component_context_->CancelComponentsLoad();

        /* Wait for all tasks to exit, but don't .Get() them - we've already caught
         * an exception, ignore the rest */
        for (auto& task : tasks) {
            if (task.IsValid()) task.Wait();
        }

        ClearComponents();
        throw;
    }

    if (is_load_cancelled) {
        ClearComponents();
        throw std::logic_error(
            "Components load cancelled, but only ComponentsLoadCancelledExceptions "
            "were caught"
        );
    }

    LOG_INFO() << "All components created. Constructors for all the components "
                  "have completed. Preparing to run OnAllComponentsLoaded "
                  "for each component.";

    try {
        component_context_->OnAllComponentsLoaded();
    } catch (const std::exception& ex) {
        ClearComponents();
        throw;
    }

    auto stop_time = std::chrono::steady_clock::now();
    load_duration_ = std::chrono::duration_cast<std::chrono::milliseconds>(stop_time - start_time);

    LOG_INFO() << "All components loaded";
}

void Manager::AddComponentImpl(
    const components::ComponentConfigMap& config_map,
    const std::string& name,
    const impl::ComponentAdderBase& adder
) {
    const auto config_it = config_map.find(name);
    if (config_it == config_map.end()) {
        throw std::runtime_error("Cannot start component " + name + ": missing config");
    }
    auto enabled = config_it->second["load-enabled"].As<bool>(true);
    if (!enabled) {
        LOG_DEBUG() << "Component " << name << " load disabled in config.yaml, skipping";
        return;
    }

    LOG_DEBUG() << "Starting component " << name;

    auto* component = component_context_->AddComponent(name, config_it->second, adder);
    if (auto* signal_processor = dynamic_cast<os_signals::ProcessorComponent*>(component))
        signal_processor_ = signal_processor;
    LOG_DEBUG() << "Started component " << name;
}

void Manager::ClearComponents() noexcept {
    {
        const std::lock_guard<std::shared_timed_mutex> lock(context_mutex_);
        components_cleared_ = true;
    }
    try {
        component_context_->ClearComponents();
    } catch (const std::exception& ex) {
        LOG_ERROR() << "error in clear components: " << ex;
    }
}

}  // namespace components::impl

USERVER_NAMESPACE_END
