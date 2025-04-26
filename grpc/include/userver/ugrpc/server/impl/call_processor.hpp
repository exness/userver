#pragma once

#include <string_view>

#include <grpcpp/server_context.h>

#include <userver/server/handlers/exceptions.hpp>

#include <userver/ugrpc/server/impl/call.hpp>
#include <userver/ugrpc/server/impl/call_kind.hpp>
#include <userver/ugrpc/server/impl/call_traits.hpp>
#include <userver/ugrpc/server/impl/exceptions.hpp>
#include <userver/ugrpc/server/impl/rpc.hpp>
#include <userver/ugrpc/server/middlewares/base.hpp>
#include <userver/ugrpc/server/result.hpp>

USERVER_NAMESPACE_BEGIN

namespace ugrpc::server::impl {

grpc::Status ReportHandlerError(const std::exception& ex, CallAnyBase& call) noexcept;

grpc::Status ReportRpcInterruptedError(CallAnyBase& call) noexcept;

grpc::Status
ReportCustomError(const USERVER_NAMESPACE::server::handlers::CustomHandlerException& ex, CallAnyBase& call) noexcept;

void WriteAccessLog(
    MiddlewareCallContext& context,
    const grpc::Status& status,
    logging::TextLoggerRef access_tskv_logger
) noexcept;

void CheckFinishStatus(bool finish_op_succeeded, const grpc::Status& status, CallAnyBase& call) noexcept;

template <typename Response>
void UnpackResult(Result<Response>&& result, std::optional<Response>& response, grpc::Status& status) {
    if (result.IsSuccess()) {
        response.emplace(result.ExtractResponse());
    } else {
        status = result.ExtractErrorStatus();
    }
}

template <typename Response>
void UnpackResult(StreamingResult<Response>&& result, std::optional<Response>& response, grpc::Status& status) {
    if (result.IsSuccess()) {
        if (result.HasLastResponse()) {
            response.emplace(result.ExtractLastResponse());
        }
    } else {
        status = result.ExtractStatus();
    }
}

template <typename CallTraits>
class CallProcessor final {
public:
    using Call = impl::Call<CallTraits>;
    using Response = typename CallTraits::Response;
    using InitialRequest = typename CallTraits::InitialRequest;
    using Context = typename CallTraits::Context;
    using ServiceBase = typename CallTraits::ServiceBase;
    using ServiceMethod = typename CallTraits::ServiceMethod;

    CallProcessor(
        Call& call,
        const Middlewares& mids,
        InitialRequest& initial_request,
        dynamic_config::Snapshot&& config_snapshot,
        logging::TextLoggerRef access_tskv_logger,
        ServiceBase& service,
        ServiceMethod service_method
    )
        : context_(utils::impl::InternalTag{}, call, std::move(config_snapshot)),
          call_(call),
          mids_(mids),
          initial_request_(initial_request),
          access_tskv_logger_(access_tskv_logger),
          service_(service),
          service_method_(service_method) {}

    void DoCall() {
        RunOnCallStart();

        // Don't keep the config snapshot for too long, especially for streaming RPCs.
        context_.ResetInitialDynamicConfig(utils::impl::InternalTag{});

        if (!Status().ok()) {
            RunOnCallFinish();
            impl::CheckFinishStatus(call_.FinishWithError(Status()), Status(), call_);
            return;
        }

        // Final response is the response sent to the client together with status in the final batch.
        std::optional<Response> final_response{};

        RunWithCatch([this, &final_response] {
            auto result = CallHandler();
            impl::UnpackResult(std::move(result), final_response, Status());
        });

        // Streaming handler can detect RPC breakage during a network interaction => IsFinished.
        // RpcFinishedEvent can signal RPC interruption while in the handler => ShouldCancel.
        if (call_.IsFinished() || engine::current_task::ShouldCancel()) {
            impl::ReportRpcInterruptedError(call_);
            // Don't run OnCallFinish.
            return;
        }

        if (!Status().ok()) {
            RunOnCallFinish();
            impl::CheckFinishStatus(call_.FinishWithError(Status()), Status(), call_);
            return;
        }

        if (final_response) {
            RunPreSendMessage(*final_response);
        }
        RunOnCallFinish();

        if (!Status().ok()) {
            impl::CheckFinishStatus(call_.FinishWithError(Status()), Status(), call_);
            return;
        }

        if constexpr (IsServerStreaming(CallTraits::kCallKind)) {
            if (!final_response) {
                impl::CheckFinishStatus(call_.Finish(), Status(), call_);
                return;
            }
        }
        UASSERT(final_response);
        impl::CheckFinishStatus(call_.Finish(*final_response), Status(), call_);
    }

private:
    auto CallHandler() {
        Context context{utils::impl::InternalTag{}, call_};

        if constexpr (impl::IsClientStreaming(CallTraits::kCallKind)) {
            return (service_.*service_method_)(context, call_);
        } else if constexpr (CallTraits::kCallKind == CallKind::kUnaryCall) {
            return (service_.*service_method_)(context, std::move(initial_request_));
        } else if constexpr (CallTraits::kCallKind == CallKind::kOutputStream) {
            return (service_.*service_method_)(context, std::move(initial_request_), call_);
        } else {
            static_assert(!sizeof(CallTraits), "Unexpected CallCategory");
        }
    }

    void RunOnCallStart() {
        UASSERT(success_pre_hooks_count_ == 0);
        for (const auto& m : mids_) {
            RunWithCatch([this, &m] { m->OnCallStart(context_); });
            if (!Status().ok()) {
                return;
            }
            // On fail, we must call OnRpcFinish only for middlewares for which OnRpcStart has been called successfully.
            // So, we watch to count of these middlewares.
            ++success_pre_hooks_count_;
            if constexpr (std::is_base_of_v<google::protobuf::Message, InitialRequest>) {
                RunWithCatch([this, m] { m->PostRecvMessage(context_, initial_request_); });
                if (!Status().ok()) {
                    return;
                }
            }
        }
    }

    void RunOnCallFinish() {
        const auto rbegin = mids_.rbegin() + (mids_.size() - success_pre_hooks_count_);
        for (auto it = rbegin; it != mids_.rend(); ++it) {
            const auto& middleware = *it;
            // We must call all OnRpcFinish despite the failures. So, don't check the status.
            RunWithCatch([this, &middleware] { middleware->OnCallFinish(context_, Status()); });
        }

        // TODO move to a middleware.
        impl::WriteAccessLog(context_, Status(), access_tskv_logger_);
    }

    void RunPreSendMessage(Response& response) {
        if constexpr (std::is_base_of_v<google::protobuf::Message, Response>) {
            // We don't want to include a heavy boost header for reverse view.
            // NOLINTNEXTLINE(modernize-loop-convert)
            for (auto it = mids_.rbegin(); it != mids_.rend(); ++it) {
                const auto& middleware = *it;
                RunWithCatch([this, &response, &middleware] { middleware->PreSendMessage(context_, response); });
                if (!Status().ok()) {
                    return;
                }
            }
        }
    }

    template <typename Func>
    void RunWithCatch(Func&& func) {
        try {
            func();
        } catch (MiddlewareRpcInterruptionError& ex) {
            Status() = ex.ExtractStatus();
        } catch (const USERVER_NAMESPACE::server::handlers::CustomHandlerException& ex) {
            Status() = impl::ReportCustomError(ex, call_);
        } catch (const RpcInterruptedError& /*ex*/) {
            UASSERT(call_.IsFinished());
            // RPC interruption will be reported below.
        } catch (const std::exception& ex) {
            Status() = impl::ReportHandlerError(ex, call_);
        }
    }

    grpc::Status& Status() { return context_.GetStatus(utils::impl::InternalTag{}); }

    MiddlewareCallContext context_;
    Call& call_;
    const Middlewares& mids_;
    // Initial request is the request which is sent to the service together with RPC initiation.
    // Unary-request RPCs have an initial request, client-streaming RPCs don't.
    InitialRequest& initial_request_;
    logging::TextLoggerRef access_tskv_logger_;
    ServiceBase& service_;
    const ServiceMethod service_method_;

    std::size_t success_pre_hooks_count_{0};
};

}  // namespace ugrpc::server::impl

USERVER_NAMESPACE_END
