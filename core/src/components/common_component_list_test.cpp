#include <userver/components/common_component_list.hpp>

#include <fmt/format.h>

#include <userver/components/run.hpp>
#include <userver/dynamic_config/test_helpers.hpp>
#include <userver/formats/json/serialize.hpp>
#include <userver/fs/blocking/temp_directory.hpp>
#include <userver/fs/blocking/write.hpp>

#include <components/component_list_test.hpp>
#include <userver/utest/utest.hpp>

USERVER_NAMESPACE_BEGIN

namespace {

constexpr std::string_view kConfigVarsTemplate = R"(
  userver-dumps-root: {0}
  dynamic-config-cache-path: {1}
  access_log_path: {0}/access.log
  access_tskv_log_path: {0}/access_tskv.log
  default_log_path: '@stderr'
  log_level: {2}
)";

// We deliberately have some defaulted options explicitly specified here, for
// testing and documentation purposes.
// clang-format off
constexpr std::string_view kStaticConfig = R"(
# /// [Sample components manager config component config]
# yaml
components_manager:
  coro_pool:
    initial_size: 50
    max_size: 50000
  default_task_processor: main-task-processor
  event_thread_pool:
    threads: 2
  task_processors:
    bg-task-processor:
      thread_name: bg-worker
      worker_threads: 2
      os-scheduling: idle
      task-processor-queue: global-task-queue
      task-trace:
        every: 1000
        max-context-switch-count: 1000
        logger: tracer
    fs-task-processor:
      thread_name: fs-worker
      worker_threads: 2
    main-task-processor:
      thread_name: main-worker
      worker_threads: 16
    monitor-task-processor:
      thread_name: monitor
      worker_threads: 2
  components:
    manager-controller:  # Nothing
# /// [Sample components manager config component config]
# /// [Sample logging configurator component config]
# yaml
    logging-configurator:
      limited-logging-enable: true
      limited-logging-interval: 1s
# /// [Sample logging configurator component config]
# /// [Sample dump configurator component config]
# yaml
    dump-configurator:
      dump-root: $userver-dumps-root
# /// [Sample dump configurator component config]
# /// [Sample testsuite support component config]
# yaml
    testsuite-support:
      testsuite-periodic-update-enabled: true
      testsuite-pg-execute-timeout: 300ms
      testsuite-pg-statement-timeout: 300ms
      testsuite-pg-readonly-master-expected: false
      testsuite-redis-timeout-connect: 5s
      testsuite-redis-timeout-single: 1s
      testsuite-redis-timeout-all: 750ms
      testsuite-increased-timeout: 40s
      cache-update-execution: concurrent
# /// [Sample testsuite support component config]
# /// [Sample http client component config]
# yaml
    http-client:
      pool-statistics-disable: false
      thread-name-prefix: http-client
      threads: 2
      fs-task-processor: fs-task-processor
      destination-metrics-auto-max-size: 100
      user-agent: common_component_list sample
      testsuite-enabled: true
      testsuite-timeout: 5s
      testsuite-allowed-url-prefixes: ['http://localhost:8083/', 'http://localhost:8084/']
# /// [Sample http client component config]
# /// [Sample dns client component config]
# yaml
    dns-client:
      fs-task-processor: fs-task-processor
      hosts-file-path: /etc/hosts
      hosts-file-update-interval: 5m
      network-timeout: 1s
      network-attempts: 1
      network-custom-servers:
        - 127.0.0.1
        - 127.0.0.2
      cache-ways: 16
      cache-size-per-way: 256
      cache-max-reply-ttl: 5m
      cache-failure-ttl: 5s
# /// [Sample dns client component config]
# /// [Sample dynamic configs client component config]
# yaml
    dynamic-config-client:
      get-configs-overrides-for-service: true
      service-name: common_component_list-service
      http-timeout: 20s
      http-retries: 5
      config-url: http://localhost:8083/
      configs-stage: $configs_stage
      fallback-to-no-proxy: false
# /// [Sample dynamic configs client component config]
# /// [Sample dynamic config client updater component config]
# yaml
    dynamic-config-client-updater:
      store-enabled: true
      load-only-my-values: true

      # options from components::CachingComponentBase
      update-types: full-and-incremental
      update-interval: 5s
      update-jitter: 2s
      full-update-interval: 5m
      first-update-fail-ok: true
      config-settings: false
      additional-cleanup-interval: 5m
# /// [Sample dynamic config client updater component config]
# /// [Sample logging component config]
# yaml
    logging:
      fs-task-processor: fs-task-processor
      loggers:
        default:
          file_path: $default_log_path
          level: $log_level
          level#fallback: debug
          overflow_behavior: discard
        access:
          file_path: $access_log_path
          overflow_behavior: discard
          format: raw
        access-tskv:
          file_path: $access_tskv_log_path
          overflow_behavior: discard
          format: raw
        tracer:
          file_path: '@stdout'
          overflow_behavior: discard
# /// [Sample logging component config]
# /// [Sample tracer component config]
# yaml
    tracer:
        service-name: config-service
        tracer: native
# /// [Sample tracer component config]
# /// [Sample statistics storage component config]
# yaml
    statistics-storage:
      # Nothing
# /// [Sample statistics storage component config]
# /// [Sample dynamic config component config]
# yaml
    dynamic-config:
      updates-enabled: true
      fs-cache-path: $dynamic-config-cache-path
      fs-task-processor: fs-task-processor
# /// [Sample dynamic config component config]
    http-client-statistics:
      fs-task-processor: fs-task-processor
# /// [Sample system statistics component config]
# yaml
    system-statistics-collector:
      fs-task-processor: fs-task-processor
      with-nginx: false
# /// [Sample system statistics component config]
)";
// clang-format on

}  // namespace

TEST_F(ComponentList, Common) {
    const auto temp_root = fs::blocking::TempDirectory::Create();
    const std::string dynamic_config_cache_path = temp_root.GetPath() + "/dynamic_config.json";
    const std::string config_vars_path = temp_root.GetPath() + "/config_vars.json";

    fs::blocking::RewriteFileContents(
        dynamic_config_cache_path, formats::json::ToString(dynamic_config::impl::GetDefaultDocsMap().AsJson())
    );

    fs::blocking::RewriteFileContents(
        config_vars_path,
        fmt::format(
            kConfigVarsTemplate,
            temp_root.GetPath(),
            dynamic_config_cache_path,
            ToString(logging::GetDefaultLoggerLevel())
        )
    );

    components::RunOnce(
        components::InMemoryConfig{std::string{kStaticConfig} + "config_vars: " + config_vars_path},
        components::CommonComponentList()
    );
}

TEST_F(ComponentList, ValidationWithConfigVars) {
    const auto temp_root = fs::blocking::TempDirectory::Create();
    const std::string dynamic_config_cache_path = temp_root.GetPath() + "/dynamic_config.json";
    const std::string config_vars_path = temp_root.GetPath() + "/config_vars.json";

    fs::blocking::RewriteFileContents(
        dynamic_config_cache_path, formats::json::ToString(dynamic_config::impl::GetDefaultDocsMap().AsJson())
    );

    fs::blocking::RewriteFileContents(
        config_vars_path,
        fmt::format(
            kConfigVarsTemplate,
            temp_root.GetPath(),
            dynamic_config_cache_path,
            ToString(logging::GetDefaultLoggerLevel())
        )
    );

    constexpr const char* kBadParam = "      non-described-in-schema-parameter: $default_log_path\n";
    const components::InMemoryConfig conf{std::string{kStaticConfig} + kBadParam + "config_vars: " + config_vars_path};

    UEXPECT_THROW_MSG(
        components::RunOnce(conf, components::CommonComponentList()),
        std::exception,
        "Error while validating static config against schema. Field "
        "'components_manager.components.system-statistics-collector.non-described-in-schema-parameter' is not declared "
        "in schema 'system-statistics-collector' (declared: load-enabled, with-nginx, fs-task-processor)"
    );
}

USERVER_NAMESPACE_END
