#pragma once

/// @file userver/ugrpc/server/exceptions.hpp
/// @brief Errors thrown by gRPC server streams

#include <exception>
#include <string>
#include <string_view>

#include <grpcpp/support/status.h>
#include <grpcpp/support/status_code_enum.h>

USERVER_NAMESPACE_BEGIN

/// Server-side utilities
namespace ugrpc::server {

/// @brief Base exception for all the server errors
class BaseError : public std::exception {
public:
    explicit BaseError(std::string message);

    const char* what() const noexcept override;

private:
    std::string message_;
};

/// @brief Error during an RPC
class RpcError : public BaseError {
public:
    RpcError(std::string_view call_name, std::string_view additional_info);
};

/// @brief RPC failed without a status. This means that either the call got
/// cancelled using `TryCancel`, the deadline has expired, or the client
/// disconnected.
class RpcInterruptedError : public RpcError {
public:
    RpcInterruptedError(std::string_view call_name, std::string_view stage);
};

/// @brief Users can throw ErrorWithStatus in their
/// RPC's implementation to return provided status code as a result.
class ErrorWithStatus : public BaseError {
public:
    explicit ErrorWithStatus(grpc::Status status);

    ErrorWithStatus(grpc::StatusCode status_code, std::string message);

    const grpc::Status& GetStatus() const;

    grpc::Status&& ExtractStatus();

private:
    grpc::Status status_;
};

}  // namespace ugrpc::server

USERVER_NAMESPACE_END
