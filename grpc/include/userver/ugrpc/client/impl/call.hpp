#pragma once

/// @file userver/ugrpc/client/impl/call.hpp
/// @brief @copybrief ugrpc::client::impl::CallAnyBase

#include <userver/tracing/span.hpp>

#include <userver/ugrpc/client/call_context.hpp>
#include <userver/ugrpc/client/impl/call_kind.hpp>
#include <userver/ugrpc/client/impl/call_state.hpp>

USERVER_NAMESPACE_BEGIN

namespace ugrpc::client::impl {

struct CallParams;
class CallState;

/// @brief Base class for any RPC
class CallAnyBase {
public:
    CallAnyBase(CallAnyBase&& other) = delete;
    CallAnyBase& operator=(CallAnyBase&& other) = delete;

    CallContext& GetContext() noexcept;

    const CallContext& GetContext() const noexcept;

protected:
    CallAnyBase(CallParams&& params, CallKind call_kind);

    // Prevent ownership via pointer to base.
    ~CallAnyBase();

    CallState& GetState() noexcept;

    const CallState& GetState() const noexcept;

private:
    CallState state_;
    CallContext context_;
};

}  // namespace ugrpc::client::impl

USERVER_NAMESPACE_END
