#include "middleware.hpp"

#include <fmt/core.h>
#include <google/protobuf/util/json_util.h>

#include <userver/logging/log_extra.hpp>
#include <userver/tracing/span.hpp>
#include <userver/utils/log.hpp>

USERVER_NAMESPACE_BEGIN

namespace ugrpc::server::middlewares::log {

namespace {

std::string ProtobufAsJsonString(const google::protobuf::Message& message,
                                 uint64_t max_msg_size) {
  google::protobuf::util::JsonPrintOptions options;
  options.add_whitespace = false;
  grpc::string as_json;
  const auto status =
      google::protobuf::util::MessageToJsonString(message, &as_json, options);
  if (!status.ok()) {
    std::string log =
        fmt::format("Error getting a json string: {}", status.ToString());
    LOG_WARNING() << log;
    return log;
  }
  return std::string{utils::log::ToLimitedUtf8(as_json, max_msg_size)};
}

std::string GetMessageForLogging(const google::protobuf::Message& message,
                                 const Middleware::Settings& settings) {
  if (logging::ShouldLog(settings.msg_log_level)) {
    return ProtobufAsJsonString(message, settings.max_msg_size);
  } else {
    return "hidden by log level";
  }
}

}  // namespace

Middleware::Middleware(const Settings& settings) : settings_(settings) {}

void Middleware::CallRequestHook(const MiddlewareCallContext& context,
                                 google::protobuf::Message& request) {
  auto& storage = context.GetCall().GetStorageContext();
  auto& span = context.GetCall().GetSpan();
  logging::LogExtra log_extra{
      {"grpc_type", "request"},
      {"body", GetMessageForLogging(request, settings_)}};

  if (storage.Get(kIsFirstRequest)) {
    storage.Set(kIsFirstRequest, false);
    log_extra.Extend("type", "request");
  }
  LOG(span.GetLogLevel()) << "gRPC request message" << std::move(log_extra);
}

void Middleware::CallResponseHook(const MiddlewareCallContext& context,
                                  google::protobuf::Message& response) {
  auto& span = context.GetCall().GetSpan();
  const auto call_kind = context.GetCall().GetCallKind();

  if (call_kind == CallKind::kUnaryCall ||
      call_kind == CallKind::kRequestStream) {
    span.AddTag("grpc_type", "response");
    span.AddNonInheritableTag("type", "response");
    span.AddNonInheritableTag("body",
                              GetMessageForLogging(response, settings_));
  } else {
    logging::LogExtra log_extra{
        {"grpc_type", "response"},
        {"body", GetMessageForLogging(response, settings_)}};
    LOG(span.GetLogLevel()) << "gRPC response message" << std::move(log_extra);
  }
}

void Middleware::Handle(MiddlewareCallContext& context) const {
  auto& storage = context.GetCall().GetStorageContext();
  const auto call_kind = context.GetCall().GetCallKind();
  storage.Emplace(kIsFirstRequest, true);

  auto& span = context.GetCall().GetSpan();
  if (settings_.local_log_level) {
    span.SetLocalLogLevel(*settings_.local_log_level);
  }
  const auto meta_type =
      fmt::format("{}/{}", context.GetServiceName(), context.GetMethodName());

  span.AddTag("meta_type", meta_type);
  if (call_kind == CallKind::kResponseStream ||
      call_kind == CallKind::kBidirectionalStream) {
    span.AddNonInheritableTag("type", "response");
    span.AddNonInheritableTag("body", "stream finished");
  }

  context.Next();
}

}  // namespace ugrpc::server::middlewares::log

USERVER_NAMESPACE_END
