#include "middleware.hpp"

#include <userver/logging/log_extra.hpp>
#include <userver/tracing/tags.hpp>

#include <ugrpc/impl/logging.hpp>

USERVER_NAMESPACE_BEGIN

namespace ugrpc::server::middlewares::log {

namespace {

std::string GetMessageForLogging(const google::protobuf::Message& message, const Settings& settings) {
    return ugrpc::impl::GetMessageForLogging(
        message,
        ugrpc::impl::MessageLoggingOptions{settings.msg_log_level, settings.max_msg_size, settings.trim_secrets}
    );
}

}  // namespace

Middleware::Middleware(const Settings& settings) : settings_(settings) {}

void Middleware::CallRequestHook(const MiddlewareCallContext& context, google::protobuf::Message& request) {
    logging::LogExtra extra{{"grpc_type", "request"}, {"body", GetMessageForLogging(request, settings_)}};
    if (context.IsClientStreaming()) {
        LOG_INFO() << "gRPC request stream message" << std::move(extra);
    } else {
        extra.Extend("type", "request");
        LOG_INFO() << "gRPC request" << std::move(extra);
    }
}

void Middleware::CallResponseHook(const MiddlewareCallContext& context, google::protobuf::Message& response) {
    logging::LogExtra extra{{"grpc_type", "response"}, {"body", GetMessageForLogging(response, settings_)}};
    if (context.IsServerStreaming()) {
        LOG_INFO() << "gRPC response stream message" << std::move(extra);
    } else {
        extra.Extend("type", "response");
        LOG_INFO() << "gRPC response" << std::move(extra);
    }
}

void Middleware::Handle(MiddlewareCallContext& context) const {
    auto& span = context.GetCall().GetSpan();

    span.AddTag("meta_type", std::string{context.GetCall().GetCallName()});
    span.AddNonInheritableTag(tracing::kSpanKind, tracing::kSpanKindServer);

    if (context.IsClientStreaming()) {
        LOG_INFO() << "gRPC request stream started" << logging::LogExtra{{"type", "request"}};
    }

    context.Next();

    if (context.IsServerStreaming()) {
        LOG_INFO() << "gRPC response stream finished" << logging::LogExtra{{"type", "response"}};
    }
}

}  // namespace ugrpc::server::middlewares::log

USERVER_NAMESPACE_END
