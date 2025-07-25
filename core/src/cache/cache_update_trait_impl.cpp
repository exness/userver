#include <cache/cache_update_trait_impl.hpp>

#include <fmt/format.h>

#include <userver/alerts/source.hpp>
#include <userver/components/component.hpp>
#include <userver/components/dump_configurator.hpp>
#include <userver/dynamic_config/source.hpp>
#include <userver/engine/task/cancel.hpp>
#include <userver/logging/log.hpp>
#include <userver/testsuite/cache_control.hpp>
#include <userver/tracing/tracer.hpp>
#include <userver/utils/algo.hpp>
#include <userver/utils/assert.hpp>
#include <userver/utils/async.hpp>
#include <userver/utils/atomic.hpp>
#include <userver/utils/datetime.hpp>
#include <userver/utils/rand.hpp>

#include <cache/cache_dependencies.hpp>
#include <dump/dump_locator.hpp>
#include <userver/dump/factory.hpp>
#include <userver/testsuite/testsuite_support.hpp>

USERVER_NAMESPACE_BEGIN

namespace cache {

namespace {

template <typename T>
T CheckNotNull(T ptr) {
    UINVARIANT(ptr, "This pointer must not be null");
    return ptr;
}

alerts::Source kCacheUpdateErrorAlert("cache_update_error");

}  // namespace

void CacheUpdateTrait::Impl::InvalidateAsync(UpdateType update_type) {
    if (static_config_.allowed_update_types == AllowedUpdateTypes::kOnlyFull &&
        update_type == UpdateType::kIncremental) {
        LOG_WARNING() << "Requested incremental update for cache '" << name_
                      << "' while only full updates were allowed";
        update_type = UpdateType::kFull;
    }

    if (update_type == UpdateType::kFull) {
        force_full_update_ = true;
    }

    auto actual = FirstUpdateInvalidation::kNo;
    first_update_invalidation_.compare_exchange_strong(actual, FirstUpdateInvalidation::kYes);
    if (actual != FirstUpdateInvalidation::kFinished) {
        // Invalidation was requested before StartPeriodicUpdates.
        return;
    }

    DoInvalidateAsync();
}

void CacheUpdateTrait::Impl::DoInvalidateAsync() {
    if (periodic_update_enabled_) {
        update_task_.ForceStepAsync();
    } else {
        if (!is_running_) {
            // InvalidateAsync outside of StartPeriodicUpdates-StopPeriodicUpdates
            // is noop, just like in production.
            return;
        }

        // We are in testsuite, update synchronously for repeatability.
        utils::CriticalAsync(task_processor_, update_task_name_, [this] {
            try {
                DoPeriodicUpdate();
            } catch (const std::exception& ex) {
                // The exception has already been logged in DoPeriodicUpdate.
                LOG_DEBUG() << "Exception from DoPeriodicUpdate of cache '" << name_ << "': " << ex;
            }
        }).Get();
    }
}

void CacheUpdateTrait::Impl::UpdateSyncDebug(UpdateType update_type) {
    const std::lock_guard lock(update_mutex_);

    if (static_config_.allowed_update_types == AllowedUpdateTypes::kOnlyFull &&
        update_type == UpdateType::kIncremental) {
        LOG_WARNING() << "Requested incremental update for cache '" << name_
                      << "' while only full updates were allowed";
        update_type = UpdateType::kFull;
    }

    const auto config = GetConfig();
    if (!config->updates_enabled) {
        LOG_INFO() << "Periodic updates are disabled for cache " << Name();
        return;
    }

    utils::CriticalAsync(task_processor_, update_task_name_, [this, update_type, &config] {
        DoUpdate(update_type, *config);
    }).Get();
}

const std::string& CacheUpdateTrait::Impl::Name() const { return name_; }

CacheUpdateTrait::Impl::Impl(CacheDependencies&& dependencies, CacheUpdateTrait& self)
    : customized_trait_(self),
      static_config_(dependencies.config),
      config_(static_config_),
      cache_control_(dependencies.cache_control),
      metrics_storage_(dependencies.metrics_storage),
      name_(std::move(dependencies.name)),
      update_task_name_("update-task/" + name_),
      task_processor_(dependencies.task_processor),
      periodic_update_enabled_(dependencies.cache_control.IsPeriodicUpdateEnabled(static_config_, name_)),
      periodic_task_flags_{utils::PeriodicTask::Flags::kChaotic},
      dumpable_(customized_trait_) {
    if (dependencies.dump_config) {
        dumper_.emplace(
            *dependencies.dump_config,
            CheckNotNull(std::move(dependencies.dump_rw_factory)),
            dependencies.fs_task_processor,
            *CheckNotNull(dependencies.config_source),
            dependencies.statistics_storage,
            dependencies.dump_control,
            dumpable_
        );
    }

    statistics_holder_ =
        dependencies.statistics_storage.RegisterWriter("cache", [this](utils::statistics::Writer& writer) {
            writer.ValueWithLabels(statistics_, {"cache_name", Name()});
        });

    if (dependencies.config.config_updates_enabled) {
        config_subscription_ =
            CheckNotNull(dependencies.config_source)->UpdateAndListen(this, "cache." + Name(), &Impl::OnConfigUpdate);
    }
}

CacheUpdateTrait::Impl::~Impl() {
    if (is_running_.load()) {
        LOG_ERROR() << "CacheUpdateTrait is being destroyed while periodic update "
                       "task is still running. "
                       "Derived class has to call StopPeriodicUpdates() in destructor. "
                    << "Component name '" << name_ << "'";
        // Don't crash in production
        UASSERT_MSG(false, "StopPeriodicUpdates() is not called");
    }
}

AllowedUpdateTypes CacheUpdateTrait::Impl::GetAllowedUpdateTypes() const {
    const auto config = config_.Read();
    return config->allowed_update_types;
}

void CacheUpdateTrait::Impl::StartPeriodicUpdates(utils::Flags<CacheUpdateTrait::Flag> flags) {
    if (is_running_.exchange(true)) {
        return;
    }

    // CacheResetRegistration is created here to achieve that cache invalidators
    // are registered in the order of cache component dependency.
    // We exploit the fact that StartPeriodicUpdates is called at the end
    // of all concrete cache component constructors.
    //
    // Registration is performed *before* the first update so that caches,
    // which indirectly wait for the artifacts of this update, are always
    // registered after this cache. This allows e.g. DynamicConfigClientUpdater
    // to always be CacheControl-updated before the caches that use
    // DynamicConfig::GetSource in their constructor.
    cache_reset_registration_ = cache_control_.RegisterPeriodicCache(customized_trait_);

    try {
        const auto config = GetConfig();

        const auto dump_time = dumper_ ? dumper_->ReadDump() : std::nullopt;
        if (dump_time) {
            last_update_ = *dump_time;
            dump_first_update_type_ =
                config->first_update_type == FirstUpdateType::kFull ? UpdateType::kFull : UpdateType::kIncremental;
        }

        if ((last_update_ == std::chrono::system_clock::time_point{} ||
             config->first_update_mode != FirstUpdateMode::kSkip) &&
            (!(flags & CacheUpdateTrait::Flag::kNoFirstUpdate) || !periodic_update_enabled_)) {
            // ignore kNoFirstUpdate if !periodic_update_enabled_
            // because some components require caches to be updated at least once

            // `InvalidateAsync` called up to this point should not result in an
            // extra update
            first_update_invalidation_ = FirstUpdateInvalidation::kNo;

            // Force first update, do it synchronously
            const tracing::Span span("first-update/" + name_);
            try {
                DoPeriodicUpdate();
            } catch (const std::exception& e) {
                if (dump_time && config->first_update_mode != FirstUpdateMode::kRequired) {
                    LOG_WARNING() << "Failed to update cache " << name_
                                  << " after loading a cache dump, going on with the "
                                     "contents loaded from the dump";
                } else if (static_config_.allow_first_update_failure) {
                    LOG_WARNING() << "Failed to update cache " << name_ << " for the first time, leaving it empty";
                } else {
                    LOG_ERROR() << "Failed to update cache " << name_ << " for the first time";
                    throw;
                }
            }
        }

        if (dump_time && config->first_update_type == FirstUpdateType::kIncrementalThenAsyncFull) {
            dump_first_update_type_ = UpdateType::kFull;
            periodic_task_flags_ |= utils::PeriodicTask::Flags::kNow;
        }

        if (config->is_strong_period) {
            periodic_task_flags_ |= utils::PeriodicTask::Flags::kStrong;
        }

        const auto first_update_invalidation = first_update_invalidation_.exchange(FirstUpdateInvalidation::kFinished);
        if (first_update_invalidation == FirstUpdateInvalidation::kYes) {
            // InvalidateAsync was called during the first update, the cache is
            // considered to already be stale.
            DoInvalidateAsync();
        }

        if (periodic_update_enabled_) {
            update_task_.Start(update_task_name_, GetPeriodicTaskSettings(*config), [this] { DoPeriodicUpdate(); });

            utils::PeriodicTask::Settings cleanup_settings(config->cleanup_interval);
            cleanup_settings.span_level = logging::Level::kNone;
            cleanup_settings.task_processor = &task_processor_;

            cleanup_task_.Start("rcu-cleanup-task/" + name_, cleanup_settings, [this] {
                config_.Cleanup();
                customized_trait_.Cleanup();
            });
        }
    } catch (...) {
        is_running_ = false;  // update_task_ is not started, don't check it in dtr
        throw;
    }
}

void CacheUpdateTrait::Impl::StopPeriodicUpdates() {
    if (!is_running_.exchange(false)) {
        return;
    }

    cache_reset_registration_.Unregister();
    config_subscription_.Unsubscribe();
    statistics_holder_.Unregister();

    try {
        update_task_.Stop();
    } catch (const std::exception& ex) {
        LOG_ERROR() << "Exception in update task of cache " << name_ << ". Reason: " << ex;
    }

    try {
        cleanup_task_.Stop();
    } catch (const std::exception& ex) {
        LOG_ERROR() << "Exception in cleanup task of cache " << name_ << ". Reason: " << ex;
    }

    if (dumper_) {
        dumper_->CancelWriteTaskAndWait();
    }
}

void CacheUpdateTrait::Impl::OnConfigUpdate(const dynamic_config::Snapshot& config) {
    const auto patch = utils::FindOptional(config[kCacheConfigSet], Name());
    config_.Assign(patch ? static_config_.MergeWith(*patch) : static_config_);
    const auto new_config = config_.Read();
    update_task_.SetSettings(GetPeriodicTaskSettings(*new_config));
    cleanup_task_.SetSettings({new_config->cleanup_interval});
}

rcu::ReadablePtr<Config> CacheUpdateTrait::Impl::GetConfig() const { return config_.Read(); }

UpdateType CacheUpdateTrait::Impl::NextUpdateType(const Config& config) {
    if (dump_first_update_type_) {
        return *dump_first_update_type_;
    }
    if (last_update_ == dump::TimePoint{}) {
        return UpdateType::kFull;
    }
    if (force_full_update_) {
        return UpdateType::kFull;
    }

    switch (config.allowed_update_types) {
        case AllowedUpdateTypes::kOnlyFull:
            return UpdateType::kFull;
        case AllowedUpdateTypes::kOnlyIncremental:
            return UpdateType::kIncremental;
        case AllowedUpdateTypes::kFullAndIncremental:
            const auto steady_now = utils::datetime::SteadyNow();
            const auto& full_update_jitter = config.full_update_jitter;
            if (!generated_full_update_jitter_ &&
                steady_now >= last_full_update_ + config.full_update_interval - full_update_jitter) {
                generated_full_update_jitter_ = std::chrono::milliseconds(
                    utils::RandRange(-full_update_jitter.count(), full_update_jitter.count() + 1)
                );
            }
            if (generated_full_update_jitter_ &&
                steady_now >= last_full_update_ + config.full_update_interval + generated_full_update_jitter_.value()) {
                return UpdateType::kFull;
            }
            return UpdateType::kIncremental;
    }

    UINVARIANT(false, "Unexpected update type");
}

void CacheUpdateTrait::Impl::DoPeriodicUpdate() {
    const std::lock_guard lock(update_mutex_);
    const auto config = GetConfig();

    const auto is_first_update = !std::exchange(first_update_attempted_, true);
    // Skip updates if they are disabled by config.
    // Ignore this skip if this is first update and cache required to be updated
    if (!config->updates_enabled && (!is_first_update || static_config_.allow_first_update_failure)) {
        LOG_INFO() << "Periodic updates are disabled for cache " << Name();
        OnUpdateSkipped();
        // TODO should use `exception_period` for the next sleep to make sure that
        // it takes the same amount of time to MarkAsExpired in case of failed
        // updates and in case of skipped updates.
        return;
    }

    const auto update_type = NextUpdateType(*config);
    try {
        DoUpdate(update_type, *config);
        // Note: "on update success" logic goes inside DoUpdate
    } catch (const std::exception& ex) {
        LOG_WARNING() << "Error while updating cache " << name_ << " (update_type=" << ToString(update_type)
                      << "). Reason: " << ex;
        throw;
    }
}

void CacheUpdateTrait::Impl::OnUpdateFailure(const Config& config) {
    OnUpdateSkipped();

    if (config.alert_on_failing_to_update_times != 0 &&
        failed_updates_counter_ >= config.alert_on_failing_to_update_times) {
        kCacheUpdateErrorAlert.FireAlert(*metrics_storage_);
        LOG_ERROR() << fmt::format("cache '{}' hasn't been updated for {} times", Name(), failed_updates_counter_);
    }
}

void CacheUpdateTrait::Impl::OnUpdateSkipped() {
    const auto failed_updates_before_expiration = static_config_.failed_updates_before_expiration;
    failed_updates_counter_++;
    if (failed_updates_counter_ == failed_updates_before_expiration) {
        customized_trait_.MarkAsExpired();
        LOG_WARNING() << "Cache is marked as expired because the number of "
                         "failed updates has reached 'failed-updates-before-expiration' ("
                      << failed_updates_before_expiration << ")";
    }
}

void CacheUpdateTrait::Impl::AssertPeriodicUpdateStarted() {
    UASSERT_MSG(
        is_running_.load(),
        "Cache " + name_ +
            " has been constructed without calling "
            "StartPeriodicUpdates(), call it in ctr"
    );
}

void CacheUpdateTrait::Impl::AssertPeriodicUpdateStopped() {
    UASSERT_MSG(
        !is_running_.load(),
        "Cache " + name_ +
            " has been destructed without calling "
            "StopPeriodicUpdates(), call it in dtr"
    );
}

void CacheUpdateTrait::Impl::OnCacheModified() { cache_modified_ = true; }

bool CacheUpdateTrait::Impl::HasPreAssignCheck() const { return static_config_.has_pre_assign_check; }

bool CacheUpdateTrait::Impl::IsSafeDataLifetime() const { return static_config_.is_safe_data_lifetime; }

void CacheUpdateTrait::Impl::SetDataSizeStatistic(std::size_t size) noexcept {
    statistics_.documents_current_count = size;
}

engine::TaskProcessor& CacheUpdateTrait::Impl::GetCacheTaskProcessor() const { return task_processor_; }

void CacheUpdateTrait::Impl::DoUpdate(UpdateType update_type, const Config& config) {
    const auto steady_now = utils::datetime::SteadyNow();
    const auto now = std::chrono::round<dump::TimePoint::duration>(utils::datetime::Now());

    const auto update_type_str = ToString(update_type);
    tracing::Span::CurrentSpan().AddTag("update_type", std::string{update_type_str});
    tracing::Span::CurrentSpan().AddTag("cache_name", name_);

    UpdateStatisticsScope stats(utils::impl::InternalTag{}, statistics_, update_type);
    LOG_INFO() << "Updating cache update_type=" << update_type_str << " name=" << name_;

    try {
        customized_trait_.Update(update_type, last_update_, now, stats);
        CheckUpdateState(stats.GetState(utils::impl::InternalTag{}), update_type_str);
    } catch (const std::exception& e) {
        OnUpdateFailure(config);
        throw;
    }

    // Update success
    if (update_type == UpdateType::kFull) {
        force_full_update_ = false;
        generated_full_update_jitter_.reset();
        last_full_update_ = steady_now;
    }
    dump_first_update_type_ = {};
    failed_updates_counter_ = 0;

    last_update_ = now;
    kCacheUpdateErrorAlert.StopAlertNow(*metrics_storage_);
    if (dumper_) {
        dumper_->OnUpdateCompleted(
            now, cache_modified_.exchange(false) ? dump::UpdateType::kModified : dump::UpdateType::kAlreadyUpToDate
        );
    }
}

void CacheUpdateTrait::Impl::CheckUpdateState(impl::UpdateState update_state, std::string_view update_type_str) {
    switch (update_state) {
        case impl::UpdateState::kNotFinished:
            // TODO add UASSERT
            LOG_ERROR() << fmt::format(
                "Cache {} has an incorrect implementation of Update "
                "method: it returned successfully, but did not update "
                "cache::UpdateStatisticsScope. Please read the docs "
                "on cache::CacheUpdateTrait::Update",
                Name()
            );
            // TODO count kNotFinished as a failure in production
            [[fallthrough]];
        case impl::UpdateState::kSuccess:
            LOG_INFO() << "Updated cache update_type=" << update_type_str << " name=" << name_;
            break;
        case impl::UpdateState::kNoChanges:
            LOG_INFO() << "No changes for cache update_type=" << update_type_str << " name=" << name_;
            break;
        case impl::UpdateState::kFailure:
            throw std::runtime_error("FinishWithError");
    }
}

utils::PeriodicTask::Settings CacheUpdateTrait::Impl::GetPeriodicTaskSettings(const Config& config) {
    utils::PeriodicTask::Settings settings{config.update_interval, config.update_jitter, periodic_task_flags_};
    settings.exception_period = config.exception_interval;
    settings.task_processor = &task_processor_;
    return settings;
}

CacheUpdateTrait::Impl::DumpableEntityProxy::DumpableEntityProxy(CacheUpdateTrait& cache) : cache_(cache) {}

void CacheUpdateTrait::Impl::DumpableEntityProxy::GetAndWrite(dump::Writer& writer) const {
    cache_.GetAndWrite(writer);
}

void CacheUpdateTrait::Impl::DumpableEntityProxy::ReadAndSet(dump::Reader& reader) { cache_.ReadAndSet(reader); }

}  // namespace cache

USERVER_NAMESPACE_END
