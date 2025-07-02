#include <userver/ugrpc/client/impl/call.hpp>

#include <userver/utils/impl/internal_tag.hpp>

USERVER_NAMESPACE_BEGIN

namespace ugrpc::client::impl {

CallContext& CallAnyBase::GetContext() noexcept { return context_; }

const CallContext& CallAnyBase::GetContext() const noexcept { return context_; }

CallAnyBase::CallAnyBase(CallParams&& params, CallKind call_kind)
    : state_(std::move(params), call_kind), context_(utils::impl::InternalTag{}, state_) {}

CallAnyBase::~CallAnyBase() = default;

CallState& CallAnyBase::GetState() noexcept { return state_; }

const CallState& CallAnyBase::GetState() const noexcept { return state_; }

}  // namespace ugrpc::client::impl

USERVER_NAMESPACE_END
