#pragma once

#include <userver/dynamic_config/snapshot.hpp>
#include <userver/dynamic_config/value.hpp>
#include <userver/yaml_config/yaml_config.hpp>

#include <userver/storages/postgres/options.hpp>

USERVER_NAMESPACE_BEGIN

namespace storages::postgres {

CommandControl Parse(const formats::json::Value& elem, formats::parse::To<CommandControl>);

ConnectionSettingsDynamic Parse(const formats::json::Value& config, formats::parse::To<ConnectionSettingsDynamic>);

ConnectionSettings Parse(const yaml_config::YamlConfig& config, formats::parse::To<ConnectionSettings>);

PoolSettingsDynamic Parse(const formats::json::Value& config, formats::parse::To<PoolSettingsDynamic>);

PoolSettings Parse(const yaml_config::YamlConfig& config, formats::parse::To<PoolSettings>);

TopologySettings Parse(const formats::json::Value& config, formats::parse::To<TopologySettings>);

TopologySettings Parse(const yaml_config::YamlConfig& config, formats::parse::To<TopologySettings>);

StatementMetricsSettings Parse(const formats::json::Value& config, formats::parse::To<StatementMetricsSettings>);

StatementMetricsSettings Parse(const yaml_config::YamlConfig& config, formats::parse::To<StatementMetricsSettings>);

ConnectionSettings::StatementLogMode
Parse(const yaml_config::YamlConfig& config, formats::parse::To<ConnectionSettings::StatementLogMode>);

struct Config final {
    static Config Parse(const dynamic_config::DocsMap& docs_map);

    CommandControl default_command_control;
    CommandControlByHandlerMap handlers_command_control;
    CommandControlByQueryMap queries_command_control;
    dynamic_config::ValueDict<PoolSettingsDynamic> pool_settings;
    dynamic_config::ValueDict<TopologySettings> topology_settings;
    dynamic_config::ValueDict<ConnectionSettingsDynamic> connection_settings;
    dynamic_config::ValueDict<StatementMetricsSettings> statement_metrics_settings;
};

extern const dynamic_config::Key<Config> kConfig;

}  // namespace storages::postgres

USERVER_NAMESPACE_END
