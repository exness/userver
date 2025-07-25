#pragma once

/// @file userver/engine/sleep.hpp
/// @brief Time-based task suspension helpers

#include <chrono>

#include <userver/engine/deadline.hpp>

USERVER_NAMESPACE_BEGIN

namespace engine {

/// @brief Suspends execution for a brief period of time, possibly allowing
/// other tasks to execute
void Yield();

/// @cond
/// Recursion stoppers/specializations
void InterruptibleSleepUntil(Deadline);
void SleepUntil(Deadline);
/// @endcond

/// Suspends execution for at least the specified amount of time or until the
/// current task is cancelled. If already cancelled, the call is equivalent to
/// Yield.
template <typename Rep, typename Period>
void InterruptibleSleepFor(const std::chrono::duration<Rep, Period>& duration) {
    engine::InterruptibleSleepUntil(Deadline::FromDuration(duration));
}

/// Suspends execution until the specified time point is reached or until the
/// current task is cancelled. If already cancelled, the call is equivalent to
/// Yield.
template <typename Clock, typename Duration>
void InterruptibleSleepUntil(const std::chrono::time_point<Clock, Duration>& time_point) {
    engine::InterruptibleSleepUntil(Deadline::FromTimePoint(time_point));
}

/// Suspends execution for at least a specified amount of time, ignores
/// cancellation. Use engine::InterruptibleSleepFor to interrupt sleep by
/// cancellation request.
template <typename Rep, typename Period>
void SleepFor(const std::chrono::duration<Rep, Period>& duration) {
    engine::SleepUntil(Deadline::FromDuration(duration));
}

/// Suspends execution until the specified time point is reached, ignores
/// cancellation. Use engine::InterruptibleSleepUntil to interrupt sleep by
/// cancellation request.
template <typename Clock, typename Duration>
void SleepUntil(const std::chrono::time_point<Clock, Duration>& time_point) {
    engine::SleepUntil(Deadline::FromTimePoint(time_point));
}

}  // namespace engine

USERVER_NAMESPACE_END
