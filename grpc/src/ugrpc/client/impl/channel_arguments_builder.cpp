#include <userver/ugrpc/client/impl/channel_arguments_builder.hpp>

#include <fmt/format.h>

#include <google/protobuf/util/json_util.h>
#include <google/protobuf/util/time_util.h>

#include <userver/formats/json/value_builder.hpp>
#include <userver/logging/log.hpp>
#include <userver/utils/algo.hpp>
#include <userver/utils/numeric_cast.hpp>

#include <userver/ugrpc/client/client_qos.hpp>
#include <userver/ugrpc/impl/to_string.hpp>
#include <userver/ugrpc/proto_json.hpp>

USERVER_NAMESPACE_BEGIN

namespace ugrpc::client::impl {

namespace {

constexpr bool HasValue(const Qos& qos) noexcept { return qos.attempts.has_value(); }

grpc::service_config::ServiceConfig ParseServiceConfig(const std::string& input) {
    grpc::service_config::ServiceConfig service_config;
    const auto status = google::protobuf::util::JsonStringToMessage(input, &service_config);
    if (!status.ok()) {
        throw std::runtime_error{fmt::format("Cannot parse ServiceConfig: {}", status.ToString())};
    }
    return service_config;
}

void SetName(grpc::service_config::MethodConfig& method_config, const grpc::service_config::MethodConfig::Name& name) {
    method_config.clear_name();
    *method_config.add_name() = name;
}

void SetName(
    grpc::service_config::MethodConfig& method_config,
    std::string_view service_name,
    std::string_view method_name
) {
    method_config.clear_name();
    auto& name = *method_config.add_name();
    name.set_service(grpc::string{service_name});
    name.set_method(grpc::string{method_name});
}

grpc::service_config::MethodConfig_RetryPolicy ConstructDefaultRetryPolicy() {
    grpc::service_config::MethodConfig_RetryPolicy retry_policy{};
    retry_policy.set_max_attempts(5);
    *retry_policy.mutable_initial_backoff() = google::protobuf::util::TimeUtil::MillisecondsToDuration(10);
    *retry_policy.mutable_max_backoff() = google::protobuf::util::TimeUtil::MillisecondsToDuration(300);
    retry_policy.set_backoff_multiplier(2.0);
    retry_policy.add_retryable_status_codes(google::rpc::UNAVAILABLE);
    return retry_policy;
}

void ApplyQosConfig(grpc::service_config::MethodConfig& method_config, const Qos& qos) {
    if (qos.attempts.has_value()) {
        if (1 == *qos.attempts) {
            method_config.clear_retry_policy();
            return;
        }

        // Values greater than 5 are treated as 5 without being considered a validation error.
        // https://github.com/grpc/proposal/blob/master/A6-client-retries.md#maximum-number-of-retries
        const auto max_attempts = utils::numeric_cast<std::uint32_t>(std::min(*qos.attempts, 5));
        UINVARIANT(1 < max_attempts, "Max attempts value must be greater than 1");
        if (!method_config.has_retry_policy()) {
            *method_config.mutable_retry_policy() = ConstructDefaultRetryPolicy();
        }
        method_config.mutable_retry_policy()->set_max_attempts(max_attempts);
    }
}

// method_config has array of names, Normalize stay only one name in array
grpc::service_config::MethodConfig Normalize(
    const grpc::service_config::MethodConfig& method_config,
    const grpc::service_config::MethodConfig::Name& name
) {
    grpc::service_config::MethodConfig result{method_config};
    SetName(result, name);
    return result;
}

grpc::service_config::MethodConfig NormalizeDefault(const grpc::service_config::MethodConfig& method_config) {
    grpc::service_config::MethodConfig result{method_config};
    SetName(result, "", "");
    return result;
}

ServiceConfigBuilder::PreparedMethodConfigs
PrepareMethodConfigs(const std::string& static_service_config, const ugrpc::impl::StaticServiceMetadata& metadata) {
    std::unordered_map<std::size_t, grpc::service_config::MethodConfig> method_configs;
    std::optional<grpc::service_config::MethodConfig> default_method_config;

    const auto service_config = ParseServiceConfig(static_service_config);
    for (const auto& method_config : service_config.method_config()) {
        for (const auto& name : method_config.name()) {
            // - If the 'service' field is empty, the 'method' field must be empty, and
            //   this MethodConfig specifies the default for all methods (it's the default
            //   config).
            if (name.service().empty()) {
                UINVARIANT(name.method().empty(), "If the 'service' field is empty, the 'method' field must be empty");
                if (!default_method_config.has_value()) {
                    default_method_config = NormalizeDefault(method_config);
                }
                continue;
            }

            if (metadata.service_full_name != name.service()) {
                throw std::runtime_error{fmt::format("Invalid MethodConfig: unknown service name {}", name.service())};
            }

            // - If the 'method' field is empty, this MethodConfig specifies the defaults
            //   for all methods for the specified service.
            if (name.method().empty()) {
                default_method_config = NormalizeDefault(method_config);
                continue;
            }

            const auto method_id = FindMethod(metadata, name.service(), name.method());
            if (!method_id.has_value()) {
                throw std::runtime_error{fmt::format("Invalid MethodConfig: unknown method name {}", name.method())};
            }

            const auto [_, inserted] = method_configs.try_emplace(*method_id, Normalize(method_config, name));
            UINVARIANT(inserted, "Each name entry must be unique across the entire ServiceConfig");
        }
    }

    return {std::move(method_configs), std::move(default_method_config)};
}

grpc::service_config::MethodConfig BuildMethodConfig(
    std::string_view service_name,
    std::string_view method_name,
    const Qos& qos,
    std::optional<grpc::service_config::MethodConfig> static_method_config
) {
    UINVARIANT(
        !service_name.empty() || method_name.empty(),
        "If the 'service' field is empty, the 'method' field must be empty"
    );

    auto method_config{std::move(static_method_config).value_or(grpc::service_config::MethodConfig{})};

    SetName(method_config, service_name, method_name);

    ApplyQosConfig(method_config, qos);

    return method_config;
}

grpc::service_config::MethodConfig
BuildDefaultMethodConfig(const Qos& qos, std::optional<grpc::service_config::MethodConfig> static_method_config) {
    return BuildMethodConfig("", "", qos, std::move(static_method_config));
}

}  // namespace

ServiceConfigBuilder::ServiceConfigBuilder(
    const ugrpc::impl::StaticServiceMetadata& metadata,
    const std::optional<std::string>& static_service_config
)
    : metadata_{metadata} {
    if (static_service_config.has_value()) {
        static_service_config_ = formats::json::FromString(*static_service_config);
        prepared_method_configs_ = PrepareMethodConfigs(*static_service_config, metadata_);
    }
}

formats::json::Value ServiceConfigBuilder::Build(const ClientQos& client_qos) const {
    formats::json::ValueBuilder service_config_builder{static_service_config_};

    if (auto method_config = BuildMethodConfigArray(client_qos); !method_config.IsEmpty()) {
        service_config_builder["methodConfig"] = std::move(method_config);
    }

    return service_config_builder.ExtractValue();
}

formats::json::Value ServiceConfigBuilder::BuildMethodConfigArray(const ClientQos& client_qos) const {
    // using only "methodConfig" field
    grpc::service_config::ServiceConfig service_config{};

    const auto qos_default = client_qos.HasDefaultValue() ? client_qos.GetDefaultValue() : std::optional<Qos>{};

    auto default_method_config = prepared_method_configs_.default_method_config;

    if (qos_default.has_value() && default_method_config.has_value()) {
        ApplyQosConfig(*default_method_config, *qos_default);
    }

    for (std::size_t method_id = 0; method_id < GetMethodsCount(metadata_); ++method_id) {
        const auto method_name = GetMethodName(metadata_, method_id);
        const auto method_full_name = GetMethodFullName(metadata_, method_id);

        const auto qos = client_qos.GetOptional(method_full_name);

        auto method_config = utils::FindOptional(prepared_method_configs_.method_configs, method_id);

        // first apply default Qos to each MethodConfig from `static-service-config`
        if (method_config.has_value() && qos_default.has_value()) {
            ApplyQosConfig(*method_config, *qos_default);
        }

        // add MethodConfig
        // if method Qos with non empty value exists,
        // or `static-service-config` has corresponding MethodConfig entry
        if (client_qos.HasValue(method_full_name) && HasValue(*qos)) {
            *service_config.add_method_config() = BuildMethodConfig(
                metadata_.service_full_name,
                method_name,
                *qos,
                method_config.has_value() ? std::move(method_config) : default_method_config
            );
        } else if (method_config.has_value()) {
            *service_config.add_method_config() = std::move(*method_config);
        }
    }

    // add default MethodConfig if default Qos with non empty value exists
    // or `static-service-config` has default MethodConfig
    if (qos_default.has_value() && HasValue(*qos_default)) {
        *service_config.add_method_config() = BuildDefaultMethodConfig(*qos_default, std::move(default_method_config));
    } else if (default_method_config.has_value()) {
        *service_config.add_method_config() = std::move(*default_method_config);
    }

    return ugrpc::MessageToJson(service_config)["methodConfig"];
}

ChannelArgumentsBuilder::ChannelArgumentsBuilder(
    const grpc::ChannelArguments& channel_args,
    const std::optional<std::string>& static_service_config,
    const ugrpc::impl::StaticServiceMetadata& metadata
)
    : channel_args_{channel_args},
      static_service_config_{static_service_config},
      service_config_builder_{metadata, static_service_config} {}

grpc::ChannelArguments ChannelArgumentsBuilder::Build(const ClientQos& client_qos) const {
    const auto service_config = service_config_builder_.Build(client_qos);
    if (service_config.IsNull()) {
        return channel_args_;
    }
    return BuildChannelArguments(channel_args_, formats::json::ToString(service_config));
}

grpc::ChannelArguments ChannelArgumentsBuilder::Build() const {
    return BuildChannelArguments(channel_args_, static_service_config_);
}

grpc::ChannelArguments
BuildChannelArguments(const grpc::ChannelArguments& channel_args, const std::optional<std::string>& service_config) {
    LOG_DEBUG() << "Building ChannelArguments, effective ServiceConfig: " << service_config;
    auto effective_channel_args{channel_args};
    if (service_config.has_value()) {
        effective_channel_args.SetServiceConfigJSON(ugrpc::impl::ToGrpcString(*service_config));
    }
    return effective_channel_args;
}

}  // namespace ugrpc::client::impl

USERVER_NAMESPACE_END
