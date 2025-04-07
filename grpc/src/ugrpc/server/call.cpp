#include <userver/ugrpc/server/call.hpp>

#include <fmt/format.h>

#include <boost/range/adaptor/reversed.hpp>

#include <userver/logging/impl/logger_base.hpp>
#include <userver/logging/log_extra.hpp>
#include <userver/logging/logger.hpp>
#include <userver/utils/algo.hpp>

#include <userver/ugrpc/impl/statistics_storage.hpp>
#include <userver/ugrpc/server/impl/exceptions.hpp>
#include <userver/ugrpc/server/middlewares/base.hpp>
#include <userver/ugrpc/status_codes.hpp>

#include <ugrpc/impl/internal_tag.hpp>
#include <ugrpc/impl/span.hpp>
#include <ugrpc/impl/status.hpp>
#include <ugrpc/server/impl/format_log_message.hpp>

USERVER_NAMESPACE_BEGIN

namespace ugrpc::server {

namespace {

void WriteAccessLog(
    logging::TextLoggerRef access_tskv_logger,
    grpc::ServerContext& context,
    std::chrono::system_clock::time_point start_time,
    std::string_view call_name,
    const grpc::Status& status
) {
    constexpr auto kLevel = logging::Level::kInfo;
    if (access_tskv_logger.ShouldLog(kLevel)) {
        logging::impl::TextLogItem log_item{impl::FormatLogMessage(
            context.client_metadata(), context.peer(), start_time, call_name, status.error_code()
        )};
        access_tskv_logger.Log(kLevel, log_item);
    }
}

void LogErrorDetails(const grpc::Status& status) {
    const auto log_level = IsServerError(status.error_code()) ? logging::Level::kError : logging::Level::kWarning;
    const auto gstatus = ugrpc::impl::ToGoogleRpcStatus(status);
    auto body =
        gstatus.has_value()
            ? fmt::format(
                  "code: {}, error message: {}\nerror details:\n{}",
                  ugrpc::ToString(status.error_code()),
                  status.error_message(),
                  ugrpc::impl::GetGStatusLimitedMessage(*gstatus)
              )
            : fmt::format("code: {}, error message: {}", ugrpc::ToString(status.error_code()), status.error_message());
    LOG(log_level) << "gRPC error" << logging::LogExtra{{"type", "response"}, {"body", std::move(body)}};
}

}  // namespace

std::string_view CallAnyBase::GetServiceName() const { return params_.service_name; }

std::string_view CallAnyBase::GetMethodName() const { return params_.method_name; }

void CallAnyBase::SetMetricsCallName(std::string_view call_name) {
    UASSERT_MSG(!call_name.empty(), "call_name must NOT be empty");
    UASSERT_MSG(call_name[0] != '/', utils::StrCat("call_name must NOT start with /, given: ", call_name));
    UASSERT_MSG(
        call_name.find('/') != std::string_view::npos, utils::StrCat("call_name must contain /, given: ", call_name)
    );

    params_.statistics.RedirectTo(params_.statistics_storage.GetGenericStatistics(call_name, std::nullopt));
}

ugrpc::impl::RpcStatisticsScope& CallAnyBase::GetStatistics(ugrpc::impl::InternalTag) { return params_.statistics; }

void CallAnyBase::ApplyRequestHook(google::protobuf::Message* request) {
    UINVARIANT(middleware_call_context_, "CallContext must be invoked");
    if (request) {
        for (const auto& middleware : params_.middlewares) {
            middleware->PostRecvMessage(*middleware_call_context_, *request);
            auto& status = middleware_call_context_->GetStatus();
            if (!status.ok()) {
                throw impl::MiddlewareRpcInterruptionError(std::move(status));
            }
        }
    }
}

void CallAnyBase::ApplyResponseHook(google::protobuf::Message* response) {
    UINVARIANT(middleware_call_context_, "CallContext must be invoked");
    if (response) {
        for (const auto& middleware : boost::adaptors::reverse(params_.middlewares)) {
            middleware->PreSendMessage(*middleware_call_context_, *response);
            auto& status = middleware_call_context_->GetStatus();
            if (!status.ok()) {
                throw impl::MiddlewareRpcInterruptionError(std::move(status));
            }
        }
    }
}

void CallAnyBase::PreSendStatus(const grpc::Status& status) noexcept {
    try {
        WriteAccessLog(
            params_.access_tskv_logger,
            params_.context,
            params_.call_span.GetStartSystemTime(),
            params_.call_name,
            status
        );

        if (!status.ok() && !impl::IsServerStreaming(call_kind_)) {
            LogErrorDetails(status);
        }
    } catch (const std::exception& ex) {
        LOG_ERROR() << "Error in CallAnyBase::PreSendStatus: " << ex;
    }
}

void CallAnyBase::PostFinish(const grpc::Status& status) noexcept {
    try {
        GetStatistics().OnExplicitFinish(status.error_code());
        ugrpc::impl::UpdateSpanWithStatus(GetSpan(), status);
    } catch (const std::exception& ex) {
        LOG_ERROR() << "Error in CallAnyBase::PostFinish: " << ex;
    }
}

}  // namespace ugrpc::server

USERVER_NAMESPACE_END
