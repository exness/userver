#include <ugrpc/server/impl/parse_config.hpp>

#include <boost/range/adaptor/transformed.hpp>

#include <userver/fs/blocking/read.hpp>
#include <userver/logging/component.hpp>
#include <userver/logging/impl/logger_base.hpp>
#include <userver/logging/level_serialization.hpp>
#include <userver/logging/null_logger.hpp>
#include <userver/storages/secdist/component.hpp>
#include <userver/utils/algo.hpp>

#include <userver/ugrpc/server/middlewares/base.hpp>

USERVER_NAMESPACE_BEGIN

namespace ugrpc::server::impl {

namespace {

constexpr std::string_view kTaskProcessorKey = "task-processor";

template <typename ParserFunc>
auto ParseOptional(
    const yaml_config::YamlConfig& service_field,
    const components::ComponentContext& context,
    ParserFunc parser_func
) {
    using ParseResult = decltype(parser_func(service_field, context));
    if (service_field.IsMissing()) {
        return boost::optional<ParseResult>{};
    }
    return boost::optional<ParseResult>{parser_func(service_field, context)};
}

template <typename Field, typename ParserFunc>
Field GetFieldOrDefault(
    const yaml_config::YamlConfig& service_field,
    const boost::optional<Field>& server_default,
    const components::ComponentContext& context,
    ParserFunc parser_func
) {
    if (service_field.IsMissing() && server_default) {
        return *server_default;
    }
    // Will throw if IsMissing and no server_default and no compile-time default.
    return parser_func(service_field, context);
}

engine::TaskProcessor&
ParseTaskProcessor(const yaml_config::YamlConfig& field, const components::ComponentContext& context) {
    return context.GetTaskProcessor(field.As<std::string>());
}

}  // namespace

ServiceDefaults
ParseServiceDefaults(const yaml_config::YamlConfig& value, const components::ComponentContext& context) {
    return ServiceDefaults{
        /*task_processor=*/ParseOptional(value[kTaskProcessorKey], context, ParseTaskProcessor),
    };
}

server::ServiceConfig ParseServiceConfig(
    const yaml_config::YamlConfig& value,
    const components::ComponentContext& context,
    const ServiceDefaults& defaults
) {
    return server::ServiceConfig{
        /*task_processor=*/GetFieldOrDefault(
            value[kTaskProcessorKey], defaults.task_processor, context, ParseTaskProcessor
        ),
        /*middlewares=*/{},
    };
}

ServerConfig ParseServerConfig(const yaml_config::YamlConfig& value) {
    ServerConfig config;
    config.unix_socket_path = value["unix-socket-path"].As<std::optional<std::string>>();
    config.port = value["port"].As<std::optional<int>>();
    config.completion_queue_num = value["completion-queue-count"].As<std::size_t>(2);
    config.channel_args = value["channel-args"].As<decltype(config.channel_args)>({});
    config.native_log_level = value["native-log-level"].As<logging::Level>(logging::Level::kError);
    config.enable_channelz = value["enable-channelz"].As<bool>(false);

    const auto ca = value["tls"]["ca"].As<std::optional<std::string>>();
    if (ca) {
        config.tls.ca = fs::blocking::ReadFileContents(ca.value());
    }
    const auto cert = value["tls"]["cert"].As<std::optional<std::string>>();
    if (cert) {
        config.tls.cert = fs::blocking::ReadFileContents(cert.value());
    }
    const auto key = value["tls"]["key"].As<std::optional<std::string>>();
    if (key) {
        config.tls.key = fs::blocking::ReadFileContents(key.value());
    }

    if (config.tls.key && !config.tls.cert) {
        throw std::runtime_error("'tls.cert' cannot be missing if 'tls.key' is set");
    }

    return config;
}

}  // namespace ugrpc::server::impl

USERVER_NAMESPACE_END
