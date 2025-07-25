#pragma once

#include <userver/utest/using_namespace_userver.hpp>

#include <userver/ugrpc/server/middlewares/base.hpp>

// Hardcoded preshared key - only for example, don't do this in production
namespace samples::grpc::auth {

extern const ::grpc::string kKey;
extern const ::grpc::string kCredentials;

}  // namespace samples::grpc::auth
