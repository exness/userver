#include <userver/ugrpc/server/impl/call_processor.hpp>

#include <chrono>

#include <userver/logging/impl/logger_base.hpp>
#include <userver/logging/log.hpp>
#include <userver/tracing/opentelemetry.hpp>
#include <userver/tracing/tags.hpp>

#include <ugrpc/server/impl/format_log_message.hpp>
#include <userver/ugrpc/impl/statistics_scope.hpp>
#include <userver/ugrpc/impl/to_string.hpp>
#include <userver/ugrpc/server/impl/error_code.hpp>
#include <userver/ugrpc/status_codes.hpp>

USERVER_NAMESPACE_BEGIN

namespace ugrpc::server::impl {

namespace {

void ReportFinishSuccess(
    const grpc::Status& status,
    tracing::Span& span,
    ugrpc::impl::RpcStatisticsScope& statistics_scope
) noexcept {
    try {
        const auto status_code = status.error_code();
        statistics_scope.OnExplicitFinish(status_code);

        span.AddTag("grpc_code", ugrpc::ToString(status_code));
        if (!status.ok()) {
            span.AddTag(tracing::kErrorFlag, true);
            span.AddTag(tracing::kErrorMessage, status.error_message());
            // TODO if Finish with error status fails, should we leave span level as INFO??
            //  Because that's what happens if ReportFinishSuccess is not called.
            span.SetLogLevel(IsServerError(status_code) ? logging::Level::kError : logging::Level::kWarning);
        }
    } catch (const std::exception& ex) {
        LOG_ERROR() << "Error in ReportFinishSuccess: " << ex;
    }
}

}  // namespace

grpc::Status ReportHandlerError(const std::exception& ex, CallAnyBase& call) noexcept {
    try {
        auto& span = call.GetSpan();
        span.AddNonInheritableTag(tracing::kErrorFlag, true);
        LOG_ERROR() << "Uncaught exception in '" << call.GetCallName() << "': " << ex;
        span.AddTag(tracing::kErrorMessage, ex.what());
        span.SetLogLevel(logging::Level::kError);
        return kUnknownErrorStatus;
    } catch (const std::exception& new_ex) {
        LOG_ERROR() << "Error in ReportHandlerError: " << new_ex;
        return grpc::Status{grpc::StatusCode::INTERNAL, ""};
    }
}

grpc::Status ReportRpcInterruptedError(CallAnyBase& call) noexcept {
    try {
        // RPC interruption leads to asynchronous task cancellation by RpcFinishedEvent,
        // so the task either is already cancelled, or is going to be cancelled.
        LOG_WARNING() << "RPC interrupted in '" << call.GetCallName()
                      << "'. The previously logged cancellation or network exception, if any, is likely caused by it.";
        call.GetStatistics().OnNetworkError();
        auto& span = call.GetSpan();
        span.AddTag(tracing::kErrorMessage, "RPC interrupted");
        span.AddTag(tracing::kErrorFlag, true);
        span.SetLogLevel(logging::Level::kWarning);
        return grpc::Status::CANCELLED;
    } catch (const std::exception& ex) {
        LOG_ERROR() << "Error in ReportRpcInterruptedError: " << ex;
        return grpc::Status{grpc::StatusCode::INTERNAL, ""};
    }
}

grpc::Status
ReportCustomError(const USERVER_NAMESPACE::server::handlers::CustomHandlerException& ex, CallAnyBase& call) noexcept {
    try {
        grpc::Status status{CustomStatusToGrpc(ex.GetCode()), ugrpc::impl::ToGrpcString(ex.GetExternalErrorBody())};

        const auto log_level = IsServerError(status.error_code()) ? logging::Level::kError : logging::Level::kWarning;
        LOG(log_level) << "Error in " << call.GetCallName() << ": " << ex;
        auto& span = call.GetSpan();
        span.AddTag(tracing::kErrorFlag, true);
        span.AddTag(tracing::kErrorMessage, ex.what());
        span.SetLogLevel(log_level);
        return status;
    } catch (const std::exception& new_ex) {
        LOG_ERROR() << "Error in ReportCustomError: " << new_ex;
        return grpc::Status{grpc::StatusCode::INTERNAL, ""};
    }
}

void WriteAccessLog(
    MiddlewareCallContext& context,
    const grpc::Status& status,
    logging::TextLoggerRef access_tskv_logger
) noexcept {
    try {
        const auto& server_context = context.GetServerContext();
        constexpr auto kLevel = logging::Level::kInfo;

        if (access_tskv_logger.ShouldLog(kLevel)) {
            logging::impl::TextLogItem log_item{FormatLogMessage(
                server_context.client_metadata(),
                server_context.peer(),
                context.GetSpan().GetStartSystemTime(),
                context.GetCallName(),
                status.error_code()
            )};
            access_tskv_logger.Log(kLevel, log_item);
        }
    } catch (const std::exception& ex) {
        LOG_ERROR() << "Error in WriteAccessLog: " << ex;
    }
}

void CheckFinishStatus(bool finish_op_succeeded, const grpc::Status& status, CallAnyBase& call) noexcept {
    if (finish_op_succeeded) {
        ReportFinishSuccess(status, call.GetSpan(), call.GetStatistics());
    } else {
        ReportRpcInterruptedError(call);
    }
}

}  // namespace ugrpc::server::impl

USERVER_NAMESPACE_END
