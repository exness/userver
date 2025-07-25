#pragma once

/// @file userver/engine/run_standalone.hpp
/// @brief @copybrief engine::RunStandalone

#include <cstddef>
#include <string>

#include <userver/utils/function_ref.hpp>

USERVER_NAMESPACE_BEGIN

namespace engine {

/// @brief A lightweight TaskProcessor config for engine::RunStandalone
struct TaskProcessorPoolsConfig final {
    std::size_t initial_coro_pool_size = 10;
    std::size_t max_coro_pool_size = 100;
    std::size_t coro_stack_size = 256 * 1024ULL;
    std::size_t ev_threads_num = 1;
    std::string ev_thread_name = "ev";
    bool ev_default_loop_disabled = false;
    bool is_stack_usage_monitor_enabled = true;
};

/// @brief Runs a payload in a temporary coroutine engine instance.
///
/// Creates a TaskProcessor with specified parameters, executes the payload
/// in an Async and exits. Mainly designated for async code unit testing.
///
/// @warning This function must not be used while another engine instance is
/// running.
///
/// @param payload Code to be run in a Task
void RunStandalone(utils::function_ref<void()> payload);

/// @overload
///
/// @param worker_threads Engine thread pool size, 1 by default
/// @param payload Code to be run in a Task
void RunStandalone(std::size_t worker_threads, utils::function_ref<void()> payload);

/// @overload
///
/// @param worker_threads Engine thread pool size, 1 by default
/// @param config A lightweight TaskProcessor config
/// @param payload Code to be run in a Task
void RunStandalone(
    std::size_t worker_threads,
    const TaskProcessorPoolsConfig& config,
    utils::function_ref<void()> payload
);

}  // namespace engine

USERVER_NAMESPACE_END
