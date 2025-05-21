#include <userver/ugrpc/client/middlewares/base.hpp>

#include <userver/ugrpc/client/impl/async_methods.hpp>

#include <ugrpc/impl/internal_tag.hpp>

USERVER_NAMESPACE_BEGIN

namespace ugrpc::client {

void MiddlewareBase::PreStartCall(MiddlewareCallContext& /*context*/) const {}

void MiddlewareBase::PostFinish(MiddlewareCallContext& /*context*/, const grpc::Status& /*status*/) const {}

void MiddlewareBase::PreSendMessage(MiddlewareCallContext& /*context*/, const google::protobuf::Message& /*message*/)
    const {}

void MiddlewareBase::PostRecvMessage(MiddlewareCallContext& /*context*/, const google::protobuf::Message& /*message*/)
    const {}

MiddlewareBase::MiddlewareBase() = default;

MiddlewareBase::~MiddlewareBase() = default;

MiddlewareCallContext::MiddlewareCallContext(impl::RpcData& data) : data_(data) {}

grpc::ClientContext& MiddlewareCallContext::GetContext() noexcept { return data_.GetContext(); }

std::string_view MiddlewareCallContext::GetClientName() const noexcept { return data_.GetClientName(); }

std::string_view MiddlewareCallContext::GetCallName() const noexcept { return data_.GetCallName(); }

tracing::Span& MiddlewareCallContext::GetSpan() noexcept { return data_.GetSpan(); }

bool MiddlewareCallContext::IsClientStreaming() const noexcept { return impl::IsClientStreaming(data_.GetCallKind()); }

bool MiddlewareCallContext::IsServerStreaming() const noexcept { return impl::IsServerStreaming(data_.GetCallKind()); }

impl::RpcData& MiddlewareCallContext::GetData(ugrpc::impl::InternalTag) { return data_; }

MiddlewarePipelineComponent::MiddlewarePipelineComponent(
    const components::ComponentConfig& config,
    const components::ComponentContext& context
)
    : USERVER_NAMESPACE::middlewares::impl::AnyMiddlewarePipelineComponent(
          config,
          context,
          /*built-in middlewares=*/
          {{
              {"grpc-client-logging", {}},
              {"grpc-client-baggage", {}},
              {"grpc-client-deadline-propagation", {}},
              {"grpc-client-headers-propagator", {}},
          }}
      ) {}

}  // namespace ugrpc::client

USERVER_NAMESPACE_END
