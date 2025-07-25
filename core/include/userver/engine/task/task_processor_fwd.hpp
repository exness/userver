#pragma once

/// @file
/// @brief @copybrief engine::TaskProcessor

#include <cstdint>
#include <functional>

/// @file userver/engine/task/task_processor_fwd.hpp
/// @brief Manages tasks execution on OS threads.

USERVER_NAMESPACE_BEGIN

/// Asynchronous engine primitives
namespace engine {

/// @brief Manages tasks execution on OS threads.
///
/// To create a task processor add its configuration to the "task_processors"
/// section of the components::ManagerControllerComponent static configuration.
class TaskProcessor;

/// @brief Get approximate count of ready-to-run tasks on the `task_processor`
std::size_t GetQueueSize(const TaskProcessor& task_processor);

/// @brief Register a function that runs on all threads on task processor
/// creation. Used for pre-initializing thread_local variables with heavy
/// constructors (constructor that does blocking system calls, file access,
/// ...):
///
/// @note It is a low-level function. You might not want to use it.
void RegisterThreadStartedHook(std::function<void()>);

}  // namespace engine

USERVER_NAMESPACE_END
