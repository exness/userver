#pragma once

#include <grpcpp/support/time.h>

#include <userver/engine/deadline.hpp>

USERVER_NAMESPACE_BEGIN

namespace ugrpc {

namespace impl {

template <typename Rep, typename Period>
engine::Deadline::Duration ToDeadlineDurationSaturating(const std::chrono::duration<Rep, Period>& duration) noexcept {
    using FromDuration = std::chrono::duration<Rep, Period>;
    using ToDuration = engine::Deadline::Duration;

    // Require resolution of 'FromDuration' higher than 'ToDuration',
    // to safely cast 'ToDuration' max/min values to 'FromDuration'

    static_assert(std::is_constructible_v<ToDuration, FromDuration>);

    if (std::chrono::duration_cast<FromDuration>(ToDuration::max()) < duration) {
        return ToDuration::max();
    }

    if (duration < std::chrono::duration_cast<FromDuration>(ToDuration::min())) {
        return ToDuration::min();
    }

    return std::chrono::duration_cast<ToDuration>(duration);
}

}  // namespace impl

/// Converts `engine::Deadline::Duration` to `gpr_timespec`.
gpr_timespec DurationToTimespec(const engine::Deadline::Duration& duration) noexcept;

template <typename Rep, typename Period>
gpr_timespec DurationToTimespec(const std::chrono::duration<Rep, Period>& duration) noexcept {
    return DurationToTimespec(impl::ToDeadlineDurationSaturating(duration));
}

/// Converts `gpr_timespec` to `engine::Deadline::Duration`
engine::Deadline::Duration TimespecToDuration(gpr_timespec t) noexcept;

/// Converts `engine::Deadline` to `gpr_timespec`.
gpr_timespec DeadlineToTimespec(const engine::Deadline& deadline) noexcept;

/// Converts `gpr_timespec` to `engine::Deadline`
engine::Deadline TimespecToDeadline(gpr_timespec t) noexcept;

}  // namespace ugrpc

USERVER_NAMESPACE_END

template <>
class grpc::TimePoint<USERVER_NAMESPACE::engine::Deadline> {
public:
    /*implicit*/ TimePoint(const USERVER_NAMESPACE::engine::Deadline& time)
        : time_(USERVER_NAMESPACE::ugrpc::DeadlineToTimespec(time)) {}

    gpr_timespec raw_time() const noexcept { return time_; }

private:
    gpr_timespec time_;
};
