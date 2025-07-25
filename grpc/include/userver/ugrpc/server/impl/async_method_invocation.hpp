#pragma once

#include <grpcpp/server_context.h>

#include <userver/engine/single_use_event.hpp>
#include <userver/engine/task/cancel.hpp>
#include <userver/ugrpc/impl/async_method_invocation.hpp>

USERVER_NAMESPACE_BEGIN

namespace ugrpc::server::impl {

class RpcFinishedEvent final : public ugrpc::impl::EventBase {
public:
    RpcFinishedEvent(engine::TaskCancellationToken cancellation_token, grpc::ServerContext& server_ctx) noexcept;

    RpcFinishedEvent(const RpcFinishedEvent&) = delete;
    RpcFinishedEvent& operator=(const RpcFinishedEvent&) = delete;
    RpcFinishedEvent(RpcFinishedEvent&&) = delete;
    RpcFinishedEvent& operator=(RpcFinishedEvent&&) = delete;

    void* GetCompletionTag() noexcept;

    /// @see EventBase::Notify
    void Notify(bool ok) noexcept override;

    /// @brief For use from coroutines
    void Wait() noexcept;

private:
    engine::TaskCancellationToken cancellation_token_;
    grpc::ServerContext& server_ctx_;
    engine::SingleUseEvent event_;
};

ugrpc::impl::AsyncMethodInvocation::WaitStatus Wait(ugrpc::impl::AsyncMethodInvocation& async) noexcept;

[[nodiscard]] bool IsInvocationSuccessful(ugrpc::impl::AsyncMethodInvocation::WaitStatus status) noexcept;

void CheckInvocationSuccessful(
    ugrpc::impl::AsyncMethodInvocation::WaitStatus status,
    std::string_view call_name,
    std::string_view stage
);

}  // namespace ugrpc::server::impl

USERVER_NAMESPACE_END
