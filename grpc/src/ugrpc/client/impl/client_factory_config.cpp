#include <ugrpc/client/impl/client_factory_config.hpp>

#include <userver/formats/yaml/serialize.hpp>
#include <userver/logging/level_serialization.hpp>
#include <userver/logging/log.hpp>
#include <userver/utils/trivial_map.hpp>
#include <userver/yaml_config/yaml_config.hpp>

#include <ugrpc/client/secdist.hpp>
#include <userver/ugrpc/impl/to_string.hpp>
#include <userver/fs/blocking/read.hpp>
#include <userver/logging/log.hpp>

USERVER_NAMESPACE_BEGIN

namespace ugrpc::client::impl {

namespace {

std::shared_ptr<grpc::ChannelCredentials> MakeCredentials(const ClientFactoryConfig& config, bool isTlsEnabled)
{
    if(isTlsEnabled && config.auth_type == AuthType::kSsl) {
        grpc::SslCredentialsOptions options;
        if(config.pem_root_certs.has_value())
        {
            options.pem_root_certs = userver::fs::blocking::ReadFileContents(config.pem_root_certs.value());
        }
        if(config.pem_private_key.has_value())
        {
            options.pem_private_key = userver::fs::blocking::ReadFileContents(config.pem_private_key.value());
        }
        if(config.pem_cert_chain.has_value())
        {
            options.pem_cert_chain = userver::fs::blocking::ReadFileContents(config.pem_cert_chain.value());
        }
            LOG_INFO()<<fmt::format("GRPC client SSL credetials initialized: pem_root_certs = {}, pem_private_key = {}, pem_cert_chain = {}",config.pem_root_certs.value_or("(undefined)"), config.pem_private_key.value_or("(undefined)"), config.pem_cert_chain.value_or("(undefined)"));
            return grpc::SslCredentials(options);
    }
    else
    {
        LOG_INFO()<<"GRPC client with non ssl initialized...";
        return grpc::InsecureChannelCredentials();
    }
}


grpc::ChannelArguments MakeChannelArgs(const yaml_config::YamlConfig& channel_args) {
    grpc::ChannelArguments args;
    if (!channel_args.IsMissing()) {
        LOG_DEBUG() << "Set client ChannelArguments: "
                    << formats::yaml::ToString(channel_args.As<formats::yaml::Value>());
        for (const auto& [key, value] : Items(channel_args)) {
            if (value.IsInt64()) {
                args.SetInt(ugrpc::impl::ToGrpcString(key), value.As<int>());
            } else {
                args.SetString(ugrpc::impl::ToGrpcString(key), value.As<std::string>());
            }
        }
    }
    return args;
}

}  // namespace

AuthType Parse(const yaml_config::YamlConfig& value, formats::parse::To<AuthType>) {
    constexpr utils::TrivialBiMap kMap([](auto selector) {
        return selector().Case(AuthType::kInsecure, "insecure").Case(AuthType::kSsl, "ssl");
    });

    return utils::ParseFromValueString(value, kMap);
}

ClientFactoryConfig Parse(const yaml_config::YamlConfig& value, formats::parse::To<ClientFactoryConfig>) {
    ClientFactoryConfig config;
    config.auth_type = value["auth-type"].As<AuthType>(AuthType::kInsecure);
    /// The buffer containing the PEM encoding of the server root certificates. If
    /// this parameter is empty, the default roots will be used.  The default
    /// roots can be overridden using the \a GRPC_DEFAULT_SSL_ROOTS_FILE_PATH
    /// environment variable pointing to a file on the file system containing the
    /// roots.
    config.pem_root_certs = value["pem-root-certs"].As<std::optional<std::string>>();
    /// The buffer containing the PEM encoding of the client's private key. This
    /// parameter can be empty if the client does not have a private key.
    config.pem_private_key = value["pem-private-key"].As<std::optional<std::string>>();
    /// The buffer containing the PEM encoding of the client's certificate chain.
    /// This parameter can be empty if the client does not have a certificate
    /// chain.
    config.pem_cert_chain = value["pem-cert-chain"].As<std::optional<std::string>>();
    config.channel_args = MakeChannelArgs(value["channel-args"]);
    config.default_service_config = value["default-service-config"].As<std::optional<std::string>>();
    config.channel_count = value["channel-count"].As<std::size_t>(config.channel_count);
    return config;
}

ClientFactorySettings
MakeFactorySettings(ClientFactoryConfig&& config, const storages::secdist::SecdistConfig* secdist, bool isTlsEnabled) {
    std::shared_ptr<grpc::ChannelCredentials> credentials = MakeCredentials(config, isTlsEnabled);
    std::unordered_map<std::string, std::shared_ptr<grpc::ChannelCredentials>> client_credentials;

    if (secdist) {
        const auto& tokens = secdist->Get<Secdist>();

        for (const auto& [client_name, token] : tokens.tokens) {
            client_credentials[client_name] = grpc::CompositeChannelCredentials(
                credentials, grpc::AccessTokenCredentials(ugrpc::impl::ToGrpcString(token))
            );
        }
    }

    return ClientFactorySettings{
        std::move(credentials),
        std::move(client_credentials),
        std::move(config.channel_args),
        std::move(config.default_service_config),
        config.channel_count,
    };
}

}  // namespace ugrpc::client::impl

USERVER_NAMESPACE_END
