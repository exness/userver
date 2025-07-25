#include <userver/clients/http/client.hpp>

#include <chrono>
#include <cstdlib>
#include <limits>

#include <moodycamel/concurrentqueue.h>

#include <userver/crypto/openssl.hpp>
#include <userver/logging/log.hpp>
#include <userver/tracing/manager.hpp>
#include <userver/utils/algo.hpp>
#include <userver/utils/async.hpp>
#include <userver/utils/rand.hpp>
#include <userver/utils/string_literal.hpp>
#include <userver/utils/userver_info.hpp>

#include <clients/http/destination_statistics.hpp>
#include <clients/http/easy_wrapper.hpp>
#include <clients/http/statistics.hpp>
#include <clients/http/testsuite.hpp>
#include <curl-ev/multi.hpp>
#include <curl-ev/ratelimit.hpp>
#include <engine/ev/thread_pool.hpp>

USERVER_NAMESPACE_BEGIN

namespace clients::http {
namespace {

constexpr utils::StringLiteral kIoThreadName = "curl";
constexpr std::chrono::minutes kEasyReinitPeriod{1};

// cURL accepts options as long, but we use size_t to avoid writing checks.
// Clamp too high values to LONG_MAX, it shouldn't matter for these magnitudes.
long ClampToLong(size_t value) { return std::min<size_t>(value, std::numeric_limits<long>::max()); }

const tracing::TracingManagerBase* GetTracingManager(const ClientSettings& settings) {
    if (settings.tracing_manager) {
        return settings.tracing_manager;
    }

    static const tracing::GenericTracingManager noop_tracing{tracing::Format{0}, tracing::Format{0}};
    return &noop_tracing;
}

}  // namespace

Client::Client(
    ClientSettings settings,
    engine::TaskProcessor& fs_task_processor,
    impl::PluginPipeline&& plugin_pipeline
)
    : deadline_propagation_config_(settings.deadline_propagation),
      cancellation_policy_(settings.cancellation_policy),
      destination_statistics_(std::make_shared<DestinationStatistics>()),
      statistics_(settings.io_threads),
      fs_task_processor_(fs_task_processor),
      user_agent_(utils::GetUserverIdentifier()),
      connect_rate_limiter_(std::make_shared<curl::ConnectRateLimiter>()),
      tracing_manager_(GetTracingManager(settings)),
      plugin_pipeline_(std::move(plugin_pipeline)) {
    const auto io_threads = settings.io_threads;
    const auto& thread_name_prefix = settings.thread_name_prefix;

    engine::ev::ThreadPoolConfig ev_config;
    ev_config.threads = io_threads;
    ev_config.thread_name.assign(
        thread_name_prefix.empty() ? std::string{kIoThreadName} : utils::StrCat(kIoThreadName, "-", thread_name_prefix)
    );
    thread_pool_ = std::make_unique<engine::ev::ThreadPool>(std::move(ev_config));

    ReinitEasy();

    multis_.reserve(io_threads);

    // libcurl synchronously reads some of /etc/* files.
    // As we want httpclient to be non-blocking, we have to shift curl's init code
    // to a fs task processor.
    engine::AsyncNoSpan(fs_task_processor_, [this, io_threads] {
        for (std::size_t i = 0; i < io_threads; ++i) {
            multis_.push_back(std::make_unique<curl::multi>(thread_pool_->NextThread(), connect_rate_limiter_));
        }
    }).Get();

    easy_reinit_task_.Start("http_easy_reinit", utils::PeriodicTask::Settings(kEasyReinitPeriod), [this] {
        ReinitEasy();
    });

    SetConfig({});
}

Client::~Client() {
    easy_reinit_task_.Stop();

    // We have to destroy *this only when all the requests are finished, because
    // otherwise `multis_` and `thread_pool_` are destroyed and pending requests
    // cause UB (e.g. segfault).
    //
    // We can not refcount *this in `EasyWrapper` because that leads to
    // destruction of `thread_pool_` in the thread of the thread pool (that's an
    // UB).
    //
    // Best solution so far: track the pending tasks and wait for them to drop to
    // zero.
    while (pending_tasks_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    while (TryDequeueIdle())
        ;

    multis_.clear();
    thread_pool_.reset();
}

Request Client::CreateRequest() {
    auto request = [this] {
        auto easy = TryDequeueIdle();
        if (easy) {
            auto idx = FindMultiIndex(easy->GetMulti());
            auto wrapper = impl::EasyWrapper{std::move(easy), *this};
            return Request{
                std::move(wrapper),
                statistics_[idx].CreateRequestStats(),
                destination_statistics_,
                resolver_,
                plugin_pipeline_,
                *tracing_manager_.GetBase()};
        } else {
            auto i = utils::RandRange(multis_.size());
            auto& multi = multis_[i];

            try {
                auto wrapper = engine::AsyncNoSpan(fs_task_processor_, [this, &multi] {
                                   return impl::EasyWrapper{easy_.Get()->GetBoundBlocking(*multi), *this};
                               }).Get();
                return Request{
                    std::move(wrapper),
                    statistics_[i].CreateRequestStats(),
                    destination_statistics_,
                    resolver_,
                    plugin_pipeline_,
                    *tracing_manager_.GetBase()};
            } catch (engine::WaitInterruptedException&) {
                throw clients::http::CancelException("wait interrupted", {}, ErrorKind::kCancel);
            } catch (engine::TaskCancelledException&) {
                throw clients::http::CancelException("task cancelled", {}, ErrorKind::kCancel);
            }
        }
    }();

    if (testsuite_config_) {
        request.SetTestsuiteConfig(testsuite_config_);
    }
    auto urls = allowed_urls_extra_.Read();
    request.SetAllowedUrlsExtra(*urls);

    if (user_agent_) {
        request.user_agent(*user_agent_);
    }

    {
        // Even if proxy is an empty string we should set it, because empty proxy
        // for CURL disables the use of *_proxy env variables.
        auto proxy_value = proxy_.Read();
        request.proxy(*proxy_value);
    }
    request.SetDeadlinePropagationConfig(deadline_propagation_config_);
    request.SetCancellationPolicy(cancellation_policy_);

    return request;
}

void Client::SetMultiplexingEnabled(bool enabled) {
    for (auto& multi : multis_) {
        multi->SetMultiplexingEnabled(enabled);
    }
}

void Client::SetMaxHostConnections(size_t max_host_connections) {
    for (auto& multi : multis_) {
        multi->SetMaxHostConnections(ClampToLong(max_host_connections));
    }
}

std::string Client::GetProxy() const { return proxy_.ReadCopy(); }

void Client::SetDnsResolver(clients::dns::Resolver* resolver) { resolver_ = resolver; }

void Client::ReinitEasy() {
    easy_.Set(utils::CriticalAsync(fs_task_processor_, "http_easy_reinit", &curl::easy::CreateBlocking).Get());
}

InstanceStatistics Client::GetMultiStatistics(size_t n) const {
    UASSERT(n < statistics_.size());
    InstanceStatistics s(statistics_[n]);

    /* Update statistics from multi in place */
    const auto& multi_stats = multis_[n]->Statistics();

    /* There is a race between close/open updates, so at least make open>=close
     * to observe non-negative current socket count. */
    s.multi.socket_close.value = multi_stats.close_socket_total();
    s.multi.socket_open.value = multi_stats.open_socket_total();
    s.multi.current_load = multi_stats.get_busy_storage().GetCurrentLoad();
    s.multi.socket_ratelimit.value = multi_stats.socket_ratelimited_total();
    return s;
}

size_t Client::FindMultiIndex(const curl::multi* multi) const {
    for (size_t i = 0; i < multis_.size(); i++) {
        if (multis_[i].get() == multi) return i;
    }
    UASSERT_MSG(false, "Unknown multi");
    throw std::logic_error("Unknown multi");
}

PoolStatistics Client::GetPoolStatistics() const {
    PoolStatistics stats;
    stats.multi.reserve(multis_.size());
    for (size_t i = 0; i < multis_.size(); i++) {
        stats.multi.push_back(GetMultiStatistics(i));
    };
    return stats;
}

void Client::SetDestinationMetricsAutoMaxSize(size_t max_size) { destination_statistics_->SetAutoMaxSize(max_size); }

const DestinationStatistics& Client::GetDestinationStatistics() const { return *destination_statistics_; }

void Client::PushIdleEasy(std::shared_ptr<curl::easy>&& easy) noexcept {
    try {
        easy->reset();
        idle_queue_->enqueue(std::move(easy));
    } catch (const std::exception& e) {
        LOG_ERROR() << e;
    }

    DecPending();
}

std::shared_ptr<curl::easy> Client::TryDequeueIdle() noexcept {
    std::shared_ptr<curl::easy> result;
    if (!idle_queue_->try_dequeue(result)) {
        return {};
    }
    return result;
}

void Client::SetTestsuiteConfig(const TestsuiteConfig& config) {
    LOG_INFO() << "http client: configured for testsuite";
    testsuite_config_ = std::make_shared<const TestsuiteConfig>(config);
}

void Client::SetAllowedUrlsExtra(std::vector<std::string>&& urls) { allowed_urls_extra_.Assign(std::move(urls)); }

void Client::SetConfig(const impl::Config& config) {
    const auto pool_size = ClampToLong(config.connection_pool_size / multis_.size());
    if (pool_size * multis_.size() != config.connection_pool_size) {
        LOG_DEBUG() << "SetConnectionPoolSize() rounded pool size for each multi (" << config.connection_pool_size
                    << "/" << multis_.size() << " rounded to " << pool_size << ")";
    }
    for (auto& multi : multis_) {
        multi->SetConnectionCacheSize(pool_size);
    }

    connect_rate_limiter_->SetGlobalHttpLimits(config.throttle.http_connect_limit, config.throttle.http_connect_rate);
    connect_rate_limiter_->SetGlobalHttpsLimits(
        config.throttle.https_connect_limit, config.throttle.https_connect_rate
    );
    connect_rate_limiter_->SetPerHostLimits(
        config.throttle.per_host_connect_limit, config.throttle.per_host_connect_rate
    );

    proxy_.Assign(config.proxy);
}

void Client::ResetUserAgent(std::optional<std::string> user_agent) { user_agent_ = std::move(user_agent); }

}  // namespace clients::http

USERVER_NAMESPACE_END
