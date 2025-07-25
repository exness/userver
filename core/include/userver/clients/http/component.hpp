#pragma once

#ifdef USERVER_TVM2_HTTP_CLIENT
#error Use components::Http from clients/http.hpp instead
#endif

/// @file userver/clients/http/component.hpp
/// @brief @copybrief components::HttpClient

#include <userver/clients/http/client.hpp>
#include <userver/components/component_base.hpp>
#include <userver/concurrent/async_event_source.hpp>
#include <userver/dynamic_config/snapshot.hpp>
#include <userver/utils/statistics/entry.hpp>

USERVER_NAMESPACE_BEGIN

namespace components {

// clang-format off

/// @ingroup userver_components
///
/// @brief Component that manages clients::http::Client.
///
/// Returned references to clients::http::Client live for a lifetime of the
/// component and are safe for concurrent use.
///
/// ## Dynamic options:
/// * @ref HTTP_CLIENT_CONNECT_THROTTLE
/// * @ref HTTP_CLIENT_CONNECTION_POOL_SIZE
/// * @ref USERVER_HTTP_PROXY
///
/// ## Static options:
/// Name | Description | Default value
/// ---- | ----------- | -------------
/// pool-statistics-disable | set to true to disable statistics for connection pool | false
/// thread-name-prefix | set OS thread name to this value | ''
/// threads | number of threads to process low level HTTP related IO system calls | 8
/// fs-task-processor | task processor to run blocking HTTP related calls, like DNS resolving or hosts reading | engine::current_task::GetBlockingTaskProcessor()
/// destination-metrics-auto-max-size | set max number of automatically created destination metrics | 100
/// user-agent | User-Agent HTTP header to show on all requests, result of utils::GetUserverIdentifier() if empty | empty
/// bootstrap-http-proxy | HTTP proxy to use at service start. Will be overridden by @ref USERVER_HTTP_PROXY at runtime config update | ''
/// testsuite-enabled | enable testsuite testing support | false
/// testsuite-timeout | if set, force the request timeout regardless of the value passed in code | -
/// testsuite-allowed-url-prefixes | if set, checks that all URLs start with any of the passed prefixes, asserts if not. Set for testing purposes only. | ''
/// dns_resolver | server hostname resolver type (getaddrinfo or async) | 'async'
/// set-deadline-propagation-header | whether to set http::common::kXYaTaxiClientTimeoutMs request header, see @ref scripts/docs/en/userver/deadline_propagation.md | true
/// plugins | Plugin names to apply. A plugin component is called "http-client-plugin-" plus the plugin name. | []
/// cancellation-policy | Cancellation policy for new requests. | cancel
///
/// ## Static configuration example:
///
/// @snippet components/common_component_list_test.cpp  Sample http client component config

// clang-format on
class HttpClient final : public ComponentBase {
public:
    /// @ingroup userver_component_names
    /// @brief The default name of components::HttpClient component
    static constexpr std::string_view kName = "http-client";

    HttpClient(const ComponentConfig&, const ComponentContext&);

    ~HttpClient() override;

    clients::http::Client& GetHttpClient();

    static yaml_config::Schema GetStaticConfigSchema();

private:
    void OnConfigUpdate(const dynamic_config::Snapshot& config);

    void WriteStatistics(utils::statistics::Writer& writer);

    static std::vector<utils::NotNull<clients::http::Plugin*>>
    FindPlugins(const std::vector<std::string>& names, const components::ComponentContext& context);

    const bool disable_pool_stats_;
    clients::http::Client http_client_;
    concurrent::AsyncEventSubscriberScope subscriber_scope_;
    utils::statistics::Entry statistics_holder_;
};

template <>
inline constexpr bool kHasValidate<HttpClient> = true;

template <>
inline constexpr auto kConfigFileMode<HttpClient> = ConfigFileMode::kNotRequired;

}  // namespace components

USERVER_NAMESPACE_END
