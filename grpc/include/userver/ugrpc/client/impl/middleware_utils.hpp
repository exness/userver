#pragma once

#include <userver/ugrpc/client/impl/async_methods.hpp>

USERVER_NAMESPACE_BEGIN

namespace ugrpc::client::impl {

struct MiddlewarePipeline {
    static void PreStartCall(RpcData& data);

    static void PreSendMessage(RpcData& data, const google::protobuf::Message& message);

    static void PostRecvMessage(RpcData& data, const google::protobuf::Message& message);

    static void PostFinish(RpcData& data, const grpc::Status& status);
};

}  // namespace ugrpc::client::impl

USERVER_NAMESPACE_END
