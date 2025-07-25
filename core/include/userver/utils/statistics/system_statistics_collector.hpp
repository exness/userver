#pragma once

/// @file userver/utils/statistics/system_statistics_collector.hpp
/// @brief @copybrief components::SystemStatisticsCollector

#include <userver/components/component_base.hpp>
#include <userver/components/component_fwd.hpp>
#include <userver/concurrent/variable.hpp>
#include <userver/engine/task/task_processor_fwd.hpp>
#include <userver/utils/periodic_task.hpp>
#include <userver/utils/statistics/entry.hpp>
#include <utils/statistics/system_statistics.hpp>

USERVER_NAMESPACE_BEGIN

namespace components {

// clang-format off

/// @ingroup userver_components
///
/// @brief Component for system resource usage statistics collection.
///
/// Periodically queries resource usage info and reports is as a set of metrics.
///
/// ## Static options:
/// Name | Description | Default value
/// ---- | ----------- | -------------
/// fs-task-processor | Task processor to use for statistics gathering | engine::current_task::GetBlockingTaskProcessor()
/// with-nginx | Whether to collect and report nginx processes statistics | false
///
/// Note that `with-nginx` is a relatively expensive option as it requires full
/// process list scan.
///
/// ## Static configuration example:
///
/// @snippet components/common_component_list_test.cpp  Sample system statistics component config

// clang-format on

class SystemStatisticsCollector final : public ComponentBase {
public:
    /// @ingroup userver_component_names
    /// @brief The default name of components::SystemStatisticsCollector
    static constexpr std::string_view kName = "system-statistics-collector";

    SystemStatisticsCollector(const ComponentConfig&, const ComponentContext&);
    ~SystemStatisticsCollector() override;

    static yaml_config::Schema GetStaticConfigSchema();

private:
    void ExtendStatistics(utils::statistics::Writer& writer);

    void ProcessTimer();

    struct Data {
        utils::statistics::impl::SystemStats last_stats{};
        utils::statistics::impl::SystemStats last_nginx_stats{};
    };

    const bool with_nginx_;
    engine::TaskProcessor& fs_task_processor_;
    utils::statistics::Entry statistics_holder_;
    concurrent::Variable<Data> data_;
    utils::PeriodicTask periodic_;
};

template <>
inline constexpr bool kHasValidate<SystemStatisticsCollector> = true;

}  // namespace components

USERVER_NAMESPACE_END
