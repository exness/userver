#include <userver/clients/http/component.hpp>

#include <curl/curlver.h>

#include <userver/clients/dns/resolver_utils.hpp>
#include <userver/components/component.hpp>
#include <userver/components/statistics_storage.hpp>
#include <userver/dynamic_config/storage/component.hpp>
#include <userver/server/middlewares/headers_propagator.hpp>
#include <userver/testsuite/testsuite_support.hpp>

#include <clients/http/destination_statistics.hpp>
#include <clients/http/statistics.hpp>
#include <clients/http/testsuite.hpp>
#include <userver/clients/http/client.hpp>
#include <userver/clients/http/config.hpp>
#include <userver/clients/http/plugin_component.hpp>
#include <userver/tracing/manager_component.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

#include <dynamic_config/variables/HTTP_CLIENT_CONNECTION_POOL_SIZE.hpp>
#include <dynamic_config/variables/HTTP_CLIENT_CONNECT_THROTTLE.hpp>
#include <dynamic_config/variables/USERVER_HTTP_PROXY.hpp>

USERVER_NAMESPACE_BEGIN

namespace components {

namespace {

constexpr size_t kDestinationMetricsAutoMaxSizeDefault = 100;
constexpr std::string_view kHttpClientPluginPrefix = "http-client-plugin-";

clients::http::ClientSettings
GetClientSettings(const ComponentConfig& component_config, const ComponentContext& context) {
    clients::http::ClientSettings settings;
    settings = component_config.As<clients::http::ClientSettings>();
    auto& tracing_locator = context.FindComponent<tracing::DefaultTracingManagerLocator>();
    settings.tracing_manager = &tracing_locator.GetTracingManager();
    settings.cancellation_policy = component_config["cancellation-policy"].As<clients::http::CancellationPolicy>(
        clients::http::CancellationPolicy::kCancel
    );
    return settings;
}

void ValidateCurlVersion() {
    const auto curr_ver = std::make_tuple(LIBCURL_VERSION_MAJOR, LIBCURL_VERSION_MINOR, LIBCURL_VERSION_PATCH);
    if (std::make_tuple(7, 88, 0) <= curr_ver && std::make_tuple(8, 1, 2) >= curr_ver) {
        // See TAXICOMMON-7844
        throw std::runtime_error("Unsupported libcurl " LIBCURL_VERSION
                                 ", versions from 7.88.0 to 8.1.2 are known to "
                                 "crash on HTTP/2 requests");
    }
}
}  // namespace

HttpClient::HttpClient(const ComponentConfig& component_config, const ComponentContext& context)
    : ComponentBase(component_config, context),
      disable_pool_stats_(component_config["pool-statistics-disable"].As<bool>(false)),
      http_client_(
          GetClientSettings(component_config, context),
          GetFsTaskProcessor(component_config, context),
          FindPlugins(component_config["plugins"].As<std::vector<std::string>>(std::vector<std::string>()), context)
      ) {
    ValidateCurlVersion();

    http_client_.SetDestinationMetricsAutoMaxSize(
        component_config["destination-metrics-auto-max-size"].As<size_t>(kDestinationMetricsAutoMaxSizeDefault)
    );

    http_client_.SetDnsResolver(clients::dns::GetResolverPtr(component_config, context));

    auto user_agent = component_config["user-agent"].As<std::optional<std::string>>();
    if (user_agent) {
        if (!user_agent->empty()) {
            http_client_.ResetUserAgent(std::move(*user_agent));
        } else {
            http_client_.ResetUserAgent({});  // no user agent
        }
    } else {
        // Leaving the default one
    }

    auto testsuite_enabled = component_config["testsuite-enabled"].As<bool>(false);
    if (testsuite_enabled) {
        const auto& timeout = component_config["testsuite-timeout"].As<std::optional<std::chrono::milliseconds>>();
        auto prefixes = component_config["testsuite-allowed-url-prefixes"].As<std::vector<std::string>>({});
        http_client_.SetTestsuiteConfig({prefixes, timeout});

        auto& testsuite = context.FindComponent<components::TestsuiteSupport>();
        testsuite.GetHttpAllowedUrlsExtra().RegisterHttpClient(http_client_);
    }

    clients::http::impl::Config bootstrap_config;
    bootstrap_config.proxy = component_config["bootstrap-http-proxy"].As<std::string>({});
    http_client_.SetConfig(bootstrap_config);

    auto& config_component = context.FindComponent<components::DynamicConfig>();
    subscriber_scope_ = components::DynamicConfig::NoblockSubscriber{config_component}.GetEventSource().AddListener(
        this, kName, &HttpClient::OnConfigUpdate
    );

    const auto thread_name_prefix = component_config["thread-name-prefix"].As<std::string>("");
    auto stats_name = "httpclient" + (thread_name_prefix.empty() ? "" : ("-" + thread_name_prefix));
    auto& storage = context.FindComponent<components::StatisticsStorage>().GetStorage();
    statistics_holder_ = storage.RegisterWriter(std::move(stats_name), [this](utils::statistics::Writer& writer) {
        return WriteStatistics(writer);
    });
}

std::vector<utils::NotNull<clients::http::Plugin*>>
HttpClient::FindPlugins(const std::vector<std::string>& names, const components::ComponentContext& context) {
    std::vector<utils::NotNull<clients::http::Plugin*>> plugins;
    for (const auto& name : names) {
        auto& component =
            context.FindComponent<clients::http::plugin::ComponentBase>(std::string{kHttpClientPluginPrefix} + name);
        plugins.emplace_back(&component.GetPlugin());
    }
    return plugins;
}

HttpClient::~HttpClient() {
    subscriber_scope_.Unsubscribe();
    statistics_holder_.Unregister();
}

clients::http::Client& HttpClient::GetHttpClient() { return http_client_; }

void HttpClient::OnConfigUpdate(const dynamic_config::Snapshot& config) {
    http_client_.SetConfig(clients::http::impl::Config{
        config[::dynamic_config::HTTP_CLIENT_CONNECTION_POOL_SIZE],
        config[::dynamic_config::USERVER_HTTP_PROXY],
        clients::http::impl::Parse(config[::dynamic_config::HTTP_CLIENT_CONNECT_THROTTLE]),
    });
}

void HttpClient::WriteStatistics(utils::statistics::Writer& writer) {
    if (!disable_pool_stats_) {
        DumpMetric(writer, http_client_.GetPoolStatistics());
    }
    DumpMetric(writer, http_client_.GetDestinationStatistics());
}

yaml_config::Schema HttpClient::GetStaticConfigSchema() {
    return yaml_config::MergeSchemas<ComponentBase>(R"(
type: object
description: Component that manages clients::http::Client.
additionalProperties: false
properties:
    pool-statistics-disable:
        type: boolean
        description: set to true to disable statistics for connection pool
        defaultDescription: false
    thread-name-prefix:
        type: string
        description: set OS thread name to this value
        defaultDescription: ''
    threads:
        type: integer
        description: number of threads to process low level HTTP related IO system calls
        defaultDescription: 8
    fs-task-processor:
        type: string
        description: task processor to run blocking HTTP related calls, like DNS resolving or hosts reading
        defaultDescription: engine::current_task::GetBlockingTaskProcessor()
    destination-metrics-auto-max-size:
        type: integer
        description: set max number of automatically created destination metrics
        defaultDescription: 100
    user-agent:
        type: string
        description: User-Agent HTTP header to show on all requests, result of utils::GetUserverIdentifier() if empty
        defaultDescription: empty
    bootstrap-http-proxy:
        type: string
        description: HTTP proxy to use at service start. Will be overridden by @ref USERVER_HTTP_PROXY at runtime config update
        defaultDescription: ''
    testsuite-enabled:
        type: boolean
        description: enable testsuite testing support
        defaultDescription: false
    testsuite-timeout:
        type: string
        description: if set, force the request timeout regardless of the value passed in code
    testsuite-allowed-url-prefixes:
        type: array
        description: if set, checks that all URLs start with any of the passed prefixes, asserts if not. Set for testing purposes only.
        items:
            type: string
            description: URL prefix
    dns_resolver:
        type: string
        description: server hostname resolver type (getaddrinfo or async)
        defaultDescription: 'async'
        enum:
          - getaddrinfo
          - async
    set-deadline-propagation-header:
        type: boolean
        description: |
            Whether to set http::common::kXYaTaxiClientTimeoutMs request header
            using the original timeout and the value of task-inherited deadline.
            Note: timeout is always updated from the task-inherited deadline
            when present.
        defaultDescription: true
    plugins:
        type: array
        description: HTTP client plugin names
        items:
            type: string
            description: plugin name
    cancellation-policy:
        type: string
        description: Cancellation policy for new requests
        enum:
          - cancel
          - ignore
)");
}

}  // namespace components

USERVER_NAMESPACE_END
