#include <userver/ugrpc/server/impl/async_methods.hpp>

USERVER_NAMESPACE_BEGIN

namespace ugrpc::server::impl {

const grpc::Status kUnimplementedStatus{grpc::StatusCode::UNIMPLEMENTED, "This method is unimplemented"};

const grpc::Status kUnknownErrorStatus{
    grpc::StatusCode::UNKNOWN,
    "The service method has exited unexpectedly, without providing a status"};

}  // namespace ugrpc::server::impl

USERVER_NAMESPACE_END
