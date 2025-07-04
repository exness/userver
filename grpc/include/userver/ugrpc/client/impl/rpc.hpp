#pragma once

/// @file userver/ugrpc/client/impl/rpc.hpp
/// @brief Classes representing an outgoing RPC

#include <exception>
#include <memory>
#include <string_view>
#include <utility>

#include <grpcpp/impl/codegen/proto_utils.h>

#include <userver/engine/deadline.hpp>
#include <userver/engine/future_status.hpp>
#include <userver/utils/assert.hpp>

#include <userver/ugrpc/client/impl/async_methods.hpp>
#include <userver/ugrpc/client/impl/call.hpp>
#include <userver/ugrpc/client/impl/call_state.hpp>
#include <userver/ugrpc/client/impl/middleware_pipeline.hpp>
#include <userver/ugrpc/client/impl/prepare_call.hpp>
#include <userver/ugrpc/client/middlewares/fwd.hpp>
#include <userver/ugrpc/time_utils.hpp>

USERVER_NAMESPACE_BEGIN

namespace ugrpc::client::impl {

// Contains the implementation of UnaryFinishFuture that is not dependent on template parameters.
class UnaryFinishFuture {
public:
    UnaryFinishFuture(CallState& state, const google::protobuf::Message* response) noexcept;

    ~UnaryFinishFuture();

    UnaryFinishFuture(UnaryFinishFuture&&) = delete;
    UnaryFinishFuture& operator=(UnaryFinishFuture&&) = delete;

    [[nodiscard]] bool IsReady() const noexcept;

    [[nodiscard]] engine::FutureStatus WaitUntil(engine::Deadline deadline) const noexcept;

    void Get();

    engine::impl::ContextAccessor* TryGetContextAccessor() noexcept;

private:
    void Destroy() noexcept;

    CallState& state_;
    const google::protobuf::Message* response_;
    mutable std::exception_ptr exception_;
};

/// @brief StreamReadFuture for waiting a single read response from stream
template <typename RPC>
class [[nodiscard]] StreamReadFuture {
public:
    /// @cond
    StreamReadFuture(
        CallState& state,
        typename RPC::RawStream& stream,
        const google::protobuf::Message* recv_message
    ) noexcept;
    /// @endcond

    StreamReadFuture(StreamReadFuture&& other) noexcept;
    StreamReadFuture& operator=(StreamReadFuture&& other) noexcept;
    StreamReadFuture(const StreamReadFuture&) = delete;
    StreamReadFuture& operator=(const StreamReadFuture&) = delete;

    ~StreamReadFuture();

    /// @brief Await response
    ///
    /// Upon completion the result is available in `response` that was
    /// specified when initiating the asynchronous read
    ///
    /// `Get` should not be called multiple times for the same StreamReadFuture.
    ///
    /// @throws ugrpc::client::RpcError on an RPC error
    /// @throws ugrpc::client::RpcCancelledError on task cancellation
    bool Get();

    /// @brief Checks if the asynchronous call has completed
    ///        Note, that once user gets result, IsReady should not be called
    /// @return true if result ready
    [[nodiscard]] bool IsReady() const noexcept;

private:
    CallState* state_{};
    typename RPC::RawStream* stream_{};
    const google::protobuf::Message* recv_message_;
};

/// @brief Controls a single request -> single response RPC
///
/// This class is not thread-safe, it cannot be used from multiple tasks at the same time.
///
/// The RPC is cancelled on destruction unless the RPC is already finished. In
/// that case the connection is not closed (it will be reused for new RPCs), and
/// the server receives `RpcInterruptedError` immediately.
template <typename Response>
class [[nodiscard]] UnaryCall final : public CallAnyBase {
    // Implementation note. For consistency with other RPC objects, UnaryCall should have been exposed to the user
    // directly. However, in 90-99% use cases it's more intuitive to treat the RPC as a future (see ResponseFuture).
    // If we decide to expose more controls, like lazy Finish or ReadInitialMetadata, then they should be added
    // to UnaryCall, and UnaryCall should be exposed via ResponseFuture::GetCall.

public:
    /// @cond
    // For internal use only
    template <typename Stub, typename Request>
    UnaryCall(
        CallParams&& params,
        PrepareUnaryCallProxy<Stub, Request, Response>&& prepare_unary_call,
        const Request& request
    );
    /// @endcond

    ~UnaryCall() = default;

    UnaryCall(UnaryCall&&) = delete;
    UnaryCall& operator=(UnaryCall&&) = delete;

    /// @brief Returns the future created earlier using @ref FinishAsync.
    UnaryFinishFuture& GetFinishFuture();

    /// @overload
    const UnaryFinishFuture& GetFinishFuture() const;

    Response Finish();

private:
    // Asynchronously finish the call.
    // `FinishAsync` should not be called multiple times for the same RPC.
    // Creates the future inside this `UnaryCall`. It can be retrieved using @ref GetFinishFuture.
    void FinishAsync();

    Response response_;
    RawResponseReader<Response> reader_;
    std::optional<UnaryFinishFuture> finish_future_;
};

/// @brief Controls a single request -> response stream RPC
///
/// This class is not thread-safe except for `GetContext`.
///
/// The RPC is cancelled on destruction unless the stream is closed (`Read` has
/// returned `false`). In that case the connection is not closed (it will be
/// reused for new RPCs), and the server receives `RpcInterruptedError`
/// immediately. gRPC provides no way to early-close a server-streaming RPC
/// gracefully.
template <typename Response>
class [[nodiscard]] InputStream final : public CallAnyBase {
public:
    /// @brief Await and read the next incoming message
    ///
    /// On end-of-input, `Finish` is called automatically.
    ///
    /// @param response where to put response on success
    /// @returns `true` on success, `false` on end-of-input, task cancellation,
    //           or if the stream is already closed for reads
    /// @throws ugrpc::client::RpcError on an RPC error
    [[nodiscard]] bool Read(Response& response);

    /// @cond
    // For internal use only
    using RawStream = grpc::ClientAsyncReader<Response>;

    template <typename Stub, typename Request>
    InputStream(
        CallParams&& params,
        PrepareServerStreamingCall<Stub, Request, Response> prepare_async_method,
        const Request& request
    );
    /// @endcond

    ~InputStream();

    InputStream(InputStream&&) = delete;
    InputStream& operator=(InputStream&&) = delete;

private:
    RawReader<Response> stream_;
};

/// @brief Controls a request stream -> single response RPC
///
/// This class is not thread-safe except for `GetContext`.
///
/// The RPC is cancelled on destruction unless `Finish` has been called. In that
/// case the connection is not closed (it will be reused for new RPCs), and the
/// server receives `RpcInterruptedError` immediately.
template <typename Request, typename Response>
class [[nodiscard]] OutputStream final : public CallAnyBase {
public:
    /// @brief Write the next outgoing message
    ///
    /// `Write` doesn't store any references to `request`, so it can be
    /// deallocated right after the call.
    ///
    /// @param request the next message to write
    /// @return true if the data is going to the wire; false if the write
    ///         operation failed (including due to task cancellation,
    //          or if the stream is already closed for writes),
    ///         in which case no more writes will be accepted,
    ///         and the error details can be fetched from Finish
    [[nodiscard]] bool Write(const Request& request);

    /// @brief Write the next outgoing message and check result
    ///
    /// `WriteAndCheck` doesn't store any references to `request`, so it can be
    /// deallocated right after the call.
    ///
    /// `WriteAndCheck` verifies result of the write and generates exception
    /// in case of issues.
    ///
    /// @param request the next message to write
    /// @throws ugrpc::client::RpcError on an RPC error
    /// @throws ugrpc::client::RpcCancelledError on task cancellation
    /// @throws ugrpc::client::RpcError if the stream is already closed for writes
    void WriteAndCheck(const Request& request);

    /// @brief Complete the RPC successfully
    ///
    /// Should be called once all the data is written. The server will then
    /// send a single `Response`.
    ///
    /// `Finish` should not be called multiple times.
    ///
    /// The connection is not closed, it will be reused for new RPCs.
    ///
    /// @returns the single `Response` received after finishing the writes
    /// @throws ugrpc::client::RpcError on an RPC error
    /// @throws ugrpc::client::RpcCancelledError on task cancellation
    Response Finish();

    /// @cond
    // For internal use only
    using RawStream = grpc::ClientAsyncWriter<Request>;

    template <typename Stub>
    OutputStream(CallParams&& params, PrepareClientStreamingCall<Stub, Request, Response> prepare_async_method);
    /// @endcond

    ~OutputStream();

    OutputStream(OutputStream&&) = delete;
    OutputStream& operator=(OutputStream&&) = delete;

private:
    Response response_;
    RawWriter<Request> stream_;
};

/// @brief Controls a request stream -> response stream RPC
///
/// It is safe to call the following methods from different coroutines:
///
///   - `GetContext`;
///   - one of (`Read`, `ReadAsync`);
///   - one of (`Write`, `WritesDone`).
///
/// `WriteAndCheck` is NOT thread-safe.
///
/// The RPC is cancelled on destruction unless the stream is closed (`Read` has
/// returned `false`). In that case the connection is not closed (it will be
/// reused for new RPCs), and the server receives `RpcInterruptedError`
/// immediately. gRPC provides no way to early-close a server-streaming RPC
/// gracefully.
///
/// `Read` and `AsyncRead` can throw if error status is received from server.
/// User MUST NOT call `Read` or `AsyncRead` again after failure of any of these
/// operations.
///
/// `Write` and `WritesDone` methods do not throw, but indicate issues with
/// the RPC by returning `false`.
///
/// `WriteAndCheck` is intended for ping-pong scenarios, when after write
/// operation the user calls `Read` and vice versa.
///
/// If `Write` or `WritesDone` returns negative result, the user MUST NOT call
/// any of these methods anymore.
/// Instead the user SHOULD call `Read` method until the end of input. If
/// `Write` or `WritesDone` finishes with negative result, finally `Read`
/// will throw an exception.
/// ## Usage example:
///
/// @snippet grpc/tests/stream_test.cpp concurrent bidirectional stream
///
template <typename Request, typename Response>
class [[nodiscard]] BidirectionalStream final : public CallAnyBase {
public:
    /// @brief Await and read the next incoming message
    ///
    /// On end-of-input, `Finish` is called automatically.
    ///
    /// @param response where to put response on success
    /// @returns `true` on success, `false` on end-of-input, task cancellation,
    ///              or if the stream is already closed for reads
    /// @throws ugrpc::client::RpcError on an RPC error
    [[nodiscard]] bool Read(Response& response);

    /// @brief Return future to read next incoming result
    ///
    /// @param response where to put response on success
    /// @return StreamReadFuture future
    /// @throws ugrpc::client::RpcError on an RPC error
    /// @throws ugrpc::client::RpcError if the stream is already closed for reads
    StreamReadFuture<BidirectionalStream> ReadAsync(Response& response);

    /// @brief Write the next outgoing message
    ///
    /// RPC will be performed immediately. No references to `request` are
    /// saved, so it can be deallocated right after the call.
    ///
    /// @param request the next message to write
    /// @return true if the data is going to the wire; false if the write
    ///         operation failed (including due to task cancellation,
    //          or if the stream is already closed for writes),
    ///         in which case no more writes will be accepted,
    ///         but Read may still have some data and status code available
    [[nodiscard]] bool Write(const Request& request);

    /// @brief Write the next outgoing message and check result
    ///
    /// `WriteAndCheck` doesn't store any references to `request`, so it can be
    /// deallocated right after the call.
    ///
    /// `WriteAndCheck` verifies result of the write and generates exception
    /// in case of issues.
    ///
    /// @param request the next message to write
    /// @throws ugrpc::client::RpcError on an RPC error
    /// @throws ugrpc::client::RpcCancelledError on task cancellation
    /// @throws ugrpc::client::RpcError if the stream is already closed for writes
    void WriteAndCheck(const Request& request);

    /// @brief Announce end-of-output to the server
    ///
    /// Should be called to notify the server and receive the final response(s).
    ///
    /// @return true if the data is going to the wire; false if the operation
    ///         failed (including if the stream is already closed for writes),
    ///         but Read may still have some data and status code available
    [[nodiscard]] bool WritesDone();

    /// @cond
    // For internal use only
    using RawStream = grpc::ClientAsyncReaderWriter<Request, Response>;

    template <typename Stub>
    BidirectionalStream(CallParams&& params, PrepareBidiStreamingCall<Stub, Request, Response> prepare_async_method);
    /// @endcond

    ~BidirectionalStream();

    BidirectionalStream(BidirectionalStream&&) = delete;
    BidirectionalStream& operator=(BidirectionalStream&&) = delete;

private:
    RawReaderWriter<Request, Response> stream_;
};

template <typename RPC>
StreamReadFuture<RPC>::StreamReadFuture(
    CallState& state,
    typename RPC::RawStream& stream,
    const google::protobuf::Message* recv_message
) noexcept
    : state_(&state), stream_(&stream), recv_message_(recv_message) {}

template <typename RPC>
StreamReadFuture<RPC>::StreamReadFuture(StreamReadFuture&& other) noexcept
    // state_ == nullptr signals that *this is empty. Other fields may remain garbage in `other`.
    : state_{std::exchange(other.state_, nullptr)}, stream_(other.stream_), recv_message_{other.recv_message_} {}

template <typename RPC>
StreamReadFuture<RPC>& StreamReadFuture<RPC>::operator=(StreamReadFuture<RPC>&& other) noexcept {
    if (this == &other) return *this;
    [[maybe_unused]] auto for_destruction = std::move(*this);
    // state_ == nullptr signals that *this is empty. Other fields may remain garbage in `other`.
    state_ = std::exchange(other.state_, nullptr);
    stream_ = other.stream_;
    recv_message_ = other.recv_message_;
    return *this;
}

template <typename RPC>
StreamReadFuture<RPC>::~StreamReadFuture() {
    if (state_) {
        // StreamReadFuture::Get wasn't called => finish RPC.
        impl::FinishAbandoned(*stream_, *state_);
    }
}

template <typename RPC>
bool StreamReadFuture<RPC>::Get() {
    UINVARIANT(state_, "'Get' must be called only once");
    const CallState::AsyncMethodInvocationGuard guard(*state_);
    auto* const state = std::exchange(state_, nullptr);
    const auto result = impl::WaitAndTryCancelIfNeeded(state->GetAsyncMethodInvocation(), state->GetClientContext());
    if (result == AsyncMethodInvocation::WaitStatus::kCancelled) {
        state->GetStatsScope().OnCancelled();
        state->GetStatsScope().Flush();
    } else if (result == AsyncMethodInvocation::WaitStatus::kError) {
        // Finish can only be called once all the data is read, otherwise the
        // underlying gRPC driver hangs.
        impl::Finish(*stream_, *state, /*final_response=*/nullptr, /*throw_on_error=*/true);
    } else {
        if (recv_message_) {
            MiddlewarePipeline::PostRecvMessage(*state, *recv_message_);
        }
    }
    return result == AsyncMethodInvocation::WaitStatus::kOk;
}

template <typename RPC>
bool StreamReadFuture<RPC>::IsReady() const noexcept {
    UINVARIANT(state_, "IsReady should be called only before 'Get'");
    auto& method = state_->GetAsyncMethodInvocation();
    return method.IsReady();
}

template <typename Response>
template <typename Stub, typename Request>
UnaryCall<Response>::UnaryCall(
    CallParams&& params,
    PrepareUnaryCallProxy<Stub, Request, Response>&& prepare_unary_call,
    const Request& request
)
    : CallAnyBase(std::move(params), CallKind::kUnaryCall) {
    MiddlewarePipeline::PreStartCall(GetState());
    if constexpr (std::is_base_of_v<google::protobuf::Message, Request>) {
        MiddlewarePipeline::PreSendMessage(GetState(), request);
    }

    reader_ = prepare_unary_call.PrepareCall(
        GetState().GetStub(), &GetState().GetClientContext(), request, &GetState().GetQueue()
    );
    reader_->StartCall();

    GetState().SetWritesFinished();

    FinishAsync();
}

template <typename Response>
void UnaryCall<Response>::FinishAsync() {
    UASSERT(reader_);

    GetState().SetFinished();
    GetState().EmplaceFinishAsyncMethodInvocation();
    auto& finish = GetState().GetFinishAsyncMethodInvocation();
    auto& status = GetState().GetStatus();
    reader_->Finish(&response_, &status, finish.GetCompletionTag());

    finish_future_.emplace(GetState(), ToBaseMessage(&response_));
}

template <typename Response>
Response UnaryCall<Response>::Finish() {
    GetFinishFuture().Get();
    return std::move(response_);
}

template <typename Response>
UnaryFinishFuture& UnaryCall<Response>::GetFinishFuture() {
    UASSERT(finish_future_);
    return *finish_future_;
}

template <typename Response>
const UnaryFinishFuture& UnaryCall<Response>::GetFinishFuture() const {
    UASSERT(finish_future_);
    return *finish_future_;
}

template <typename Response>
template <typename Stub, typename Request>
InputStream<Response>::InputStream(
    CallParams&& params,
    PrepareServerStreamingCall<Stub, Request, Response> prepare_async_method,
    const Request& request
)
    : CallAnyBase(std::move(params), CallKind::kInputStream) {
    MiddlewarePipeline::PreStartCall(GetState());
    MiddlewarePipeline::PreSendMessage(GetState(), request);

    // NOLINTNEXTLINE(cppcoreguidelines-prefer-member-initializer)
    stream_ = impl::PrepareCall(
        prepare_async_method, GetState().GetStub(), &GetState().GetClientContext(), request, &GetState().GetQueue()
    );
    impl::StartCall(*stream_, GetState());

    GetState().SetWritesFinished();
}

template <typename Response>
InputStream<Response>::~InputStream() {
    impl::FinishAbandoned(*stream_, GetState());
}

template <typename Response>
bool InputStream<Response>::Read(Response& response) {
    if (!GetState().IsReadAvailable()) {
        // If the stream is already finished, we must exit immediately.
        // If not, even the middlewares may access something that is already dead.
        return false;
    }

    if (impl::Read(*stream_, response, GetState())) {
        MiddlewarePipeline::PostRecvMessage(GetState(), response);
        return true;
    } else {
        // Finish can only be called once all the data is read, otherwise the
        // underlying gRPC driver hangs.
        impl::Finish(*stream_, GetState(), /*final_response=*/nullptr, /*throw_on_error=*/true);
        return false;
    }
}

template <typename Request, typename Response>
template <typename Stub>
OutputStream<Request, Response>::OutputStream(
    CallParams&& params,
    PrepareClientStreamingCall<Stub, Request, Response> prepare_async_method
)
    : CallAnyBase(std::move(params), CallKind::kOutputStream) {
    MiddlewarePipeline::PreStartCall(GetState());

    // 'response_' will be filled upon successful 'Finish' async call
    // NOLINTNEXTLINE(cppcoreguidelines-prefer-member-initializer)
    stream_ = impl::PrepareCall(
        prepare_async_method, GetState().GetStub(), &GetState().GetClientContext(), &response_, &GetState().GetQueue()
    );
    impl::StartCall(*stream_, GetState());
}

template <typename Request, typename Response>
OutputStream<Request, Response>::~OutputStream() {
    impl::FinishAbandoned(*stream_, GetState());
}

template <typename Request, typename Response>
bool OutputStream<Request, Response>::Write(const Request& request) {
    if (!GetState().IsWriteAvailable()) {
        // If the stream is already finished, we must exit immediately.
        // If not, even the middlewares may access something that is already dead.
        return false;
    }

    MiddlewarePipeline::PreSendMessage(GetState(), request);

    // Don't buffer writes, otherwise in an event subscription scenario, events
    // may never actually be delivered
    const grpc::WriteOptions write_options{};
    return impl::Write(*stream_, request, write_options, GetState());
}

template <typename Request, typename Response>
void OutputStream<Request, Response>::WriteAndCheck(const Request& request) {
    if (!GetState().IsWriteAndCheckAvailable()) {
        // If the stream is already finished, we must exit immediately.
        // If not, even the middlewares may access something that is already dead.
        throw RpcError(GetState().GetCallName(), "'WriteAndCheck' called on a finished or closed stream");
    }

    MiddlewarePipeline::PreSendMessage(GetState(), request);

    // Don't buffer writes, otherwise in an event subscription scenario, events
    // may never actually be delivered
    const grpc::WriteOptions write_options{};
    if (!impl::Write(*stream_, request, write_options, GetState())) {
        // We don't need final_response here, because the RPC is broken anyway.
        impl::Finish(*stream_, GetState(), /*final_response=*/nullptr, /*throw_on_error=*/true);
    }
}

template <typename Request, typename Response>
Response OutputStream<Request, Response>::Finish() {
    // gRPC does not implicitly call `WritesDone` in `Finish`,
    // contrary to the documentation
    if (GetState().IsWriteAvailable()) {
        impl::WritesDone(*stream_, GetState());
    }

    impl::Finish(*stream_, GetState(), ToBaseMessage(&response_), /*throw_on_error=*/true);

    return std::move(response_);
}

template <typename Request, typename Response>
template <typename Stub>
BidirectionalStream<Request, Response>::BidirectionalStream(
    CallParams&& params,
    PrepareBidiStreamingCall<Stub, Request, Response> prepare_async_method
)
    : CallAnyBase(std::move(params), CallKind::kBidirectionalStream) {
    MiddlewarePipeline::PreStartCall(GetState());

    // NOLINTNEXTLINE(cppcoreguidelines-prefer-member-initializer)
    stream_ = impl::PrepareCall(
        prepare_async_method, GetState().GetStub(), &GetState().GetClientContext(), &GetState().GetQueue()
    );
    impl::StartCall(*stream_, GetState());
}

template <typename Request, typename Response>
BidirectionalStream<Request, Response>::~BidirectionalStream() {
    impl::FinishAbandoned(*stream_, GetState());
}

template <typename Request, typename Response>
StreamReadFuture<BidirectionalStream<Request, Response>> BidirectionalStream<Request, Response>::ReadAsync(
    Response& response
) {
    if (!GetState().IsReadAvailable()) {
        // If the stream is already finished, we must exit immediately.
        // If not, even the middlewares may access something that is already dead.
        throw RpcError(GetState().GetCallName(), "'ReadAsync' called on a finished call");
    }

    impl::ReadAsync(*stream_, response, GetState());
    return StreamReadFuture<BidirectionalStream<Request, Response>>{GetState(), *stream_, ToBaseMessage(&response)};
}

template <typename Request, typename Response>
bool BidirectionalStream<Request, Response>::Read(Response& response) {
    if (!GetState().IsReadAvailable()) {
        return false;
    }

    auto future = ReadAsync(response);
    return future.Get();
}

template <typename Request, typename Response>
bool BidirectionalStream<Request, Response>::Write(const Request& request) {
    if (!GetState().IsWriteAvailable()) {
        // If the stream is already finished, we must exit immediately.
        // If not, even the middlewares may access something that is already dead.
        return false;
    }

    MiddlewarePipeline::PreSendMessage(GetState(), request);

    // Don't buffer writes, optimize for ping-pong-style interaction
    const grpc::WriteOptions write_options{};
    return impl::Write(*stream_, request, write_options, GetState());
}

template <typename Request, typename Response>
void BidirectionalStream<Request, Response>::WriteAndCheck(const Request& request) {
    if (!GetState().IsWriteAndCheckAvailable()) {
        // If the stream is already finished, we must exit immediately.
        // If not, even the middlewares may access something that is already dead.
        throw RpcError(GetState().GetCallName(), "'WriteAndCheck' called on a finished or closed stream");
    }

    MiddlewarePipeline::PreSendMessage(GetState(), request);

    // Don't buffer writes, optimize for ping-pong-style interaction
    const grpc::WriteOptions write_options{};
    impl::WriteAndCheck(*stream_, request, write_options, GetState());
}

template <typename Request, typename Response>
bool BidirectionalStream<Request, Response>::WritesDone() {
    if (!GetState().IsWriteAvailable()) {
        // If the stream is already finished, we must exit immediately.
        // If not, even the middlewares may access something that is already dead.
        return false;
    }

    return impl::WritesDone(*stream_, GetState());
}

}  // namespace ugrpc::client::impl

USERVER_NAMESPACE_END
