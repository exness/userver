#pragma once

#include <chrono>
#include <optional>

#include <grpcpp/client_context.h>

#include <userver/ugrpc/impl/async_method_invocation.hpp>

USERVER_NAMESPACE_BEGIN

namespace ugrpc::client::impl {

/// AsyncMethodInvocation for Finish method that stops stats and Span timers
/// ASAP, without waiting for a Task to wake up
class FinishAsyncMethodInvocation final : public ugrpc::impl::AsyncMethodInvocation {
public:
    void Notify(bool ok) noexcept override;

    /// When notify is called, we store timestamp and later use it in statistics
    [[nodiscard]] std::chrono::steady_clock::time_point GetFinishTime() const;

private:
    std::optional<std::chrono::steady_clock::time_point> finish_time_;
};

}  // namespace ugrpc::client::impl

USERVER_NAMESPACE_END
