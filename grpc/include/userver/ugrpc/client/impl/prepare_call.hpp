#pragma once

#include <grpcpp/generic/generic_stub.h>

#include <userver/ugrpc/client/impl/stub_handle.hpp>

USERVER_NAMESPACE_BEGIN

namespace ugrpc::client::impl {

template <typename F, class Stub, typename... Args>
decltype(auto) PrepareCall(StubHandle& stub_handle, F Stub::*prepare_async_method, Args&&... args) {
    auto& stub = stub_handle.Get<Stub>();
    return std::invoke(prepare_async_method, stub, std::forward<Args>(args)...);
}

template <typename F>
decltype(auto) PrepareCall(
    StubHandle& stub_handle,
    F grpc::GenericStub::*prepare_call,
    grpc::ClientContext* context,
    const grpc::ByteBuffer& request,
    grpc::CompletionQueue* cq,
    const grpc::string& method_name
) {
    auto& generic_stub = stub_handle.Get<grpc::GenericStub>();
    return std::invoke(prepare_call, generic_stub, context, method_name, request, cq);
}

}  // namespace ugrpc::client::impl

USERVER_NAMESPACE_END
