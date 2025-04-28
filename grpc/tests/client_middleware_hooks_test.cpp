#include <userver/utest/utest.hpp>

#include <userver/engine/sleep.hpp>

#include <tests/client_middleware_base_gmock.hpp>
#include <tests/middlewares_fixture.hpp>
#include <tests/unit_test_client.usrv.pb.hpp>
#include <tests/unit_test_service.usrv.pb.hpp>

USERVER_NAMESPACE_BEGIN

namespace {

class UnitTestService final : public sample::ugrpc::UnitTestServiceBase {
public:
    SayHelloResult SayHello(CallContext& /*context*/, sample::ugrpc::GreetingRequest&& request) override {
        SleepIfNeeded();
        if (fail_status_.has_value()) {
            return *fail_status_;
        }

        sample::ugrpc::GreetingResponse response;
        response.set_name("Hello " + request.name());
        return response;
    }

    ReadManyResult ReadMany(
        CallContext& /*context*/,
        sample::ugrpc::StreamGreetingRequest&& request,
        ReadManyWriter& writer
    ) override {
        SleepIfNeeded();

        sample::ugrpc::StreamGreetingResponse response;
        response.set_name("Hello again " + request.name());
        for (int i = 0; i < request.number(); ++i) {
            response.set_number(i);
            writer.Write(response);

            if (fail_status_.has_value()) {
                return *fail_status_;
            }
        }

        return grpc::Status::OK;
    }

    WriteManyResult WriteMany(CallContext& /*context*/, WriteManyReader& reader) override {
        SleepIfNeeded();
        if (fail_status_.has_value()) {
            return *fail_status_;
        }

        sample::ugrpc::StreamGreetingRequest request;
        int count = 0;
        while (reader.Read(request)) {
            ++count;
        }
        sample::ugrpc::StreamGreetingResponse response;
        response.set_name("Hello");
        response.set_number(count);
        return response;
    }

    ChatResult Chat(CallContext& /*context*/, ChatReaderWriter& stream) override {
        SleepIfNeeded();

        sample::ugrpc::StreamGreetingRequest request;
        sample::ugrpc::StreamGreetingResponse response;
        int count = 0;
        while (stream.Read(request)) {
            ++count;
            response.set_number(count);
            response.set_name("Hello " + request.name());
            stream.Write(response);

            if (fail_status_.has_value()) {
                return *fail_status_;
            }
        }
        return grpc::Status::OK;
    }

    void SetResponsesBlocked(bool blocked) { responses_blocked_ = blocked; }

    void StartFailingWithStatus(grpc::StatusCode code) { fail_status_.emplace(code, "call error"); }
    void StopFailingWithStatus() { fail_status_.reset(); }

private:
    void SleepIfNeeded() {
        if (responses_blocked_) {
            engine::InterruptibleSleepFor(utest::kMaxTestWaitTime);
        }
    }

    bool responses_blocked_{false};
    std::optional<grpc::Status> fail_status_;
};

using ClientMiddlewaresHooksTest = tests::MiddlewaresFixture<
    tests::client::ClientMiddlewareBaseMock,
    UnitTestService,
    sample::ugrpc::UnitTestServiceClient,
    /*N=*/1>;

}  // namespace

UTEST_F(ClientMiddlewaresHooksTest, HappyPathUnary) {
    EXPECT_CALL(Middleware(0), PreStartCall).Times(1);
    EXPECT_CALL(Middleware(0), PreSendMessage).Times(1);
    EXPECT_CALL(Middleware(0), PostRecvMessage).Times(1);
    EXPECT_CALL(Middleware(0), PostFinish).Times(1);

    sample::ugrpc::GreetingRequest request;
    request.set_name("userver");
    auto response = Client().SayHello(request);

    EXPECT_EQ(response.name(), "Hello userver");
}

UTEST_F(ClientMiddlewaresHooksTest, HappyPathClientStreaming) {
    constexpr std::size_t kMessages{3};

    EXPECT_CALL(Middleware(0), PreStartCall).Times(1);
    EXPECT_CALL(Middleware(0), PreSendMessage).Times(kMessages);
    EXPECT_CALL(Middleware(0), PostRecvMessage).Times(1);
    EXPECT_CALL(Middleware(0), PostFinish).Times(1);

    sample::ugrpc::StreamGreetingRequest request;
    request.set_name("userver");
    auto stream = Client().WriteMany();

    for (std::size_t message{1}; message <= kMessages; ++message) {
        EXPECT_TRUE(stream.Write(request));
    }
    const auto response = stream.Finish();

    EXPECT_EQ(response.name(), "Hello");
    EXPECT_EQ(response.number(), kMessages);
}

UTEST_F(ClientMiddlewaresHooksTest, HappyPathServerStreaming) {
    constexpr std::size_t kMessages{3};

    EXPECT_CALL(Middleware(0), PreStartCall).Times(1);
    EXPECT_CALL(Middleware(0), PreSendMessage).Times(1);
    EXPECT_CALL(Middleware(0), PostRecvMessage).Times(kMessages);
    EXPECT_CALL(Middleware(0), PostFinish).Times(1);

    sample::ugrpc::StreamGreetingRequest request;
    request.set_name("userver");
    request.set_number(kMessages);
    auto stream = Client().ReadMany(request);

    sample::ugrpc::StreamGreetingResponse response;
    std::size_t message{0};
    while (stream.Read(response)) {
        EXPECT_EQ(response.number(), message);

        message += 1;
    }
    EXPECT_EQ(message, kMessages);
}

UTEST_F(ClientMiddlewaresHooksTest, HappyPathBidirectionalStreaming) {
    constexpr std::size_t kMessages{3};

    EXPECT_CALL(Middleware(0), PreStartCall).Times(1);
    EXPECT_CALL(Middleware(0), PreSendMessage).Times(kMessages);
    EXPECT_CALL(Middleware(0), PostRecvMessage).Times(kMessages);
    EXPECT_CALL(Middleware(0), PostFinish).Times(1);

    auto stream = Client().Chat();

    sample::ugrpc::StreamGreetingRequest request;
    sample::ugrpc::StreamGreetingResponse response;
    // NOLINTBEGIN(clang-analyzer-optin.cplusplus.UninitializedObject)
    for (std::size_t message{1}; message <= kMessages; ++message) {
        request.set_number(message);

        stream.WriteAndCheck(request);

        EXPECT_TRUE(stream.Read(response));
        EXPECT_EQ(response.number(), message);
    }
    // NOLINTEND(clang-analyzer-optin.cplusplus.UninitializedObject)
    EXPECT_TRUE(stream.WritesDone());
    EXPECT_FALSE(stream.Read(response));
}

UTEST_F(ClientMiddlewaresHooksTest, HappyPathDetailedUnary) {
    ::testing::MockFunction<void(std::string_view checkpoint_name)> checkpoint;
    {
        ::testing::InSequence s;

        // Pre* called after request created
        EXPECT_CALL(Middleware(0), PreStartCall).Times(1);
        EXPECT_CALL(Middleware(0), PreSendMessage).Times(1);
        EXPECT_CALL(Middleware(0), PostRecvMessage).Times(0);
        EXPECT_CALL(Middleware(0), PostFinish).Times(0);
        EXPECT_CALL(checkpoint, Call("AfterCallStart"));

        // Post* called after Finish
        EXPECT_CALL(Middleware(0), PreStartCall).Times(0);
        EXPECT_CALL(Middleware(0), PreSendMessage).Times(0);
        EXPECT_CALL(Middleware(0), PostRecvMessage).Times(1);
        EXPECT_CALL(Middleware(0), PostFinish).Times(1);
        EXPECT_CALL(checkpoint, Call("AfterCallDone"));
    }

    sample::ugrpc::GreetingRequest request;
    request.set_name("userver");

    // Pre* called after request created
    auto future = Client().AsyncSayHello(request);
    checkpoint.Call("AfterCallStart");

    const auto status = future.WaitUntil(engine::Deadline::FromDuration(utest::kMaxTestWaitTime));
    EXPECT_EQ(status, engine::FutureStatus::kReady);

    const auto response = future.Get();
    EXPECT_EQ(response.name(), "Hello userver");
    checkpoint.Call("AfterCallDone");
}

UTEST_F(ClientMiddlewaresHooksTest, HappyPathDetailedClientStreaming) {
    constexpr std::size_t kMessages{3};

    ::testing::MockFunction<void(std::string_view checkpoint_name)> checkpoint;
    {
        ::testing::InSequence s;

        // Pre* called on stream init
        EXPECT_CALL(Middleware(0), PreStartCall).Times(1);
        EXPECT_CALL(checkpoint, Call("AfterCallStart"));

        // PreSend called on each Write
        for (std::size_t i{0}; i < kMessages; ++i) {
            EXPECT_CALL(Middleware(0), PreSendMessage).Times(1);
            EXPECT_CALL(checkpoint, Call("AfterWrite"));
        }

        // Post* called after Finish
        EXPECT_CALL(Middleware(0), PostRecvMessage).Times(1);
        EXPECT_CALL(Middleware(0), PostFinish).Times(1);
        EXPECT_CALL(checkpoint, Call("AfterCallFinish"));
    }

    sample::ugrpc::StreamGreetingRequest request;
    request.set_name("userver");

    auto stream = Client().WriteMany();
    checkpoint.Call("AfterCallStart");

    for (std::size_t message{1}; message <= kMessages; ++message) {
        EXPECT_TRUE(stream.Write(request));
        checkpoint.Call("AfterWrite");
    }

    const auto response = stream.Finish();
    EXPECT_EQ(response.name(), "Hello");
    EXPECT_EQ(response.number(), kMessages);
    checkpoint.Call("AfterCallFinish");
}

UTEST_F(ClientMiddlewaresHooksTest, HappyPathDetailedServerStreaming) {
    constexpr std::size_t kMessages{3};

    ::testing::MockFunction<void(std::string_view checkpoint_name)> checkpoint;
    {
        ::testing::InSequence s;

        // Pre* called on stream init
        EXPECT_CALL(Middleware(0), PreStartCall).Times(1);
        EXPECT_CALL(Middleware(0), PreSendMessage).Times(1);
        EXPECT_CALL(checkpoint, Call("AfterCallStart"));

        // PostRecv called on each Read
        for (std::size_t i{0}; i < kMessages; ++i) {
            EXPECT_CALL(Middleware(0), PostRecvMessage).Times(1);
            EXPECT_CALL(checkpoint, Call("AfterRead"));
        }

        // PostFinish called after Read from completed rpc
        EXPECT_CALL(Middleware(0), PostFinish).Times(1);
        EXPECT_CALL(checkpoint, Call("AfterFinalRead"));
    }

    sample::ugrpc::StreamGreetingRequest request;
    request.set_name("userver");
    request.set_number(kMessages);
    sample::ugrpc::StreamGreetingResponse response;

    auto stream = Client().ReadMany(request);
    checkpoint.Call("AfterCallStart");

    for (std::size_t message{0}; message < kMessages; ++message) {
        EXPECT_TRUE(stream.Read(response));
        checkpoint.Call("AfterRead");

        EXPECT_EQ(response.number(), message);
    }

    EXPECT_FALSE(stream.Read(response));
    checkpoint.Call("AfterFinalRead");
}

UTEST_F(ClientMiddlewaresHooksTest, HappyPathDetailedBidirectionalStreaming) {
    constexpr std::size_t kMessages{3};

    ::testing::MockFunction<void(std::string_view checkpoint_name)> checkpoint;
    {
        ::testing::InSequence s;

        // PreStart called on stream init
        EXPECT_CALL(Middleware(0), PreStartCall).Times(1);
        EXPECT_CALL(checkpoint, Call("AfterCallStart"));

        // PreSend called on each Write
        // PreRecv called on each Read
        for (std::size_t i{0}; i < kMessages; ++i) {
            EXPECT_CALL(Middleware(0), PreSendMessage).Times(1);
            EXPECT_CALL(checkpoint, Call("AfterWrite"));

            EXPECT_CALL(Middleware(0), PostRecvMessage).Times(1);
            EXPECT_CALL(checkpoint, Call("AfterRead"));
        }

        // PostFinish called after Read from completed rpc
        EXPECT_CALL(Middleware(0), PostFinish).Times(1);
        EXPECT_CALL(checkpoint, Call("AfterFinalRead"));
    }

    sample::ugrpc::StreamGreetingRequest request;
    sample::ugrpc::StreamGreetingResponse response;

    auto stream = Client().Chat();
    checkpoint.Call("AfterCallStart");

    // NOLINTBEGIN(clang-analyzer-optin.cplusplus.UninitializedObject)
    for (std::size_t message{1}; message <= kMessages; ++message) {
        request.set_number(message);

        stream.WriteAndCheck(request);
        checkpoint.Call("AfterWrite");

        EXPECT_TRUE(stream.Read(response));
        checkpoint.Call("AfterRead");

        EXPECT_EQ(response.number(), message);
    }
    // NOLINTEND(clang-analyzer-optin.cplusplus.UninitializedObject)

    EXPECT_TRUE(stream.WritesDone());
    EXPECT_FALSE(stream.Read(response));

    checkpoint.Call("AfterFinalRead");
}

UTEST_F(ClientMiddlewaresHooksTest, MiddlewareExceptionUnaryPreStart) {
    EXPECT_CALL(Middleware(0), PreStartCall).Times(1);
    EXPECT_CALL(Middleware(0), PreSendMessage).Times(0);
    EXPECT_CALL(Middleware(0), PostRecvMessage).Times(0);
    EXPECT_CALL(Middleware(0), PostFinish).Times(0);

    ON_CALL(Middleware(0), PreStartCall).WillByDefault([](const ugrpc::client::MiddlewareCallContext&) {
        throw std::runtime_error{"mock error"};
    });

    sample::ugrpc::GreetingRequest request;
    request.set_name("userver");
    UEXPECT_THROW(auto future = Client().AsyncSayHello(request), std::runtime_error);
}

UTEST_F(ClientMiddlewaresHooksTest, MiddlewareExceptionUnaryPreSend) {
    EXPECT_CALL(Middleware(0), PreStartCall).Times(1);
    EXPECT_CALL(Middleware(0), PreSendMessage).Times(1);
    EXPECT_CALL(Middleware(0), PostRecvMessage).Times(0);
    EXPECT_CALL(Middleware(0), PostFinish).Times(0);

    ON_CALL(Middleware(0), PreSendMessage)
        .WillByDefault([](const ugrpc::client::MiddlewareCallContext&, const google::protobuf::Message&) {
            throw std::runtime_error{"mock error"};
        });

    sample::ugrpc::GreetingRequest request;
    request.set_name("userver");
    UEXPECT_THROW(auto future = Client().AsyncSayHello(request), std::runtime_error);
}

UTEST_F(ClientMiddlewaresHooksTest, MiddlewareExceptionUnaryPostRecv) {
    EXPECT_CALL(Middleware(0), PreStartCall).Times(1);
    EXPECT_CALL(Middleware(0), PreSendMessage).Times(1);
    EXPECT_CALL(Middleware(0), PostRecvMessage).Times(1);
    EXPECT_CALL(Middleware(0), PostFinish).Times(0);

    ON_CALL(Middleware(0), PostRecvMessage)
        .WillByDefault([](const ugrpc::client::MiddlewareCallContext&, const google::protobuf::Message&) {
            throw std::runtime_error{"mock error"};
        });

    sample::ugrpc::GreetingRequest request;
    request.set_name("userver");
    std::optional<sample::ugrpc::UnitTestServiceClient::SayHelloResponseFuture> future;
    UEXPECT_NO_THROW(future.emplace(Client().AsyncSayHello(request)));

    UEXPECT_THROW(future->Get(), std::runtime_error);
}

UTEST_F(ClientMiddlewaresHooksTest, MiddlewareExceptionUnaryPostFinish) {
    EXPECT_CALL(Middleware(0), PreStartCall).Times(1);
    EXPECT_CALL(Middleware(0), PreSendMessage).Times(1);
    EXPECT_CALL(Middleware(0), PostRecvMessage).Times(1);
    EXPECT_CALL(Middleware(0), PostFinish).Times(1);

    ON_CALL(Middleware(0), PostFinish)
        .WillByDefault([](const ugrpc::client::MiddlewareCallContext&, const grpc::Status&) {
            throw std::runtime_error{"mock error"};
        });

    sample::ugrpc::GreetingRequest request;
    request.set_name("userver");
    std::optional<sample::ugrpc::UnitTestServiceClient::SayHelloResponseFuture> future;
    UEXPECT_NO_THROW(future.emplace(Client().AsyncSayHello(request)));

    UEXPECT_THROW(future->Get(), std::runtime_error);
}

UTEST_F(ClientMiddlewaresHooksTest, MiddlewareExceptionClientStreaming) {
    EXPECT_CALL(Middleware(0), PreStartCall).Times(1);
    EXPECT_CALL(Middleware(0), PreSendMessage).Times(0);
    EXPECT_CALL(Middleware(0), PostRecvMessage).Times(0);
    EXPECT_CALL(Middleware(0), PostFinish).Times(0);

    ON_CALL(Middleware(0), PreStartCall).WillByDefault([](const ugrpc::client::MiddlewareCallContext&) {
        throw std::runtime_error{"mock error"};
    });

    UEXPECT_THROW(auto future = Client().WriteMany(), std::runtime_error);
}

UTEST_F(ClientMiddlewaresHooksTest, MiddlewareExceptionServerStreaming) {
    EXPECT_CALL(Middleware(0), PreStartCall).Times(1);
    EXPECT_CALL(Middleware(0), PreSendMessage).Times(0);
    EXPECT_CALL(Middleware(0), PostRecvMessage).Times(0);
    EXPECT_CALL(Middleware(0), PostFinish).Times(0);

    ON_CALL(Middleware(0), PreStartCall).WillByDefault([](const ugrpc::client::MiddlewareCallContext&) {
        throw std::runtime_error{"mock error"};
    });

    sample::ugrpc::StreamGreetingRequest request;
    request.set_name("userver");
    UEXPECT_THROW(auto future = Client().ReadMany(request), std::runtime_error);
}

UTEST_F(ClientMiddlewaresHooksTest, MiddlewareExceptionBidirectionalStreaming) {
    EXPECT_CALL(Middleware(0), PreStartCall).Times(1);
    EXPECT_CALL(Middleware(0), PreSendMessage).Times(0);
    EXPECT_CALL(Middleware(0), PostRecvMessage).Times(0);
    EXPECT_CALL(Middleware(0), PostFinish).Times(0);

    ON_CALL(Middleware(0), PreStartCall).WillByDefault([](const ugrpc::client::MiddlewareCallContext&) {
        throw std::runtime_error{"mock error"};
    });

    UEXPECT_THROW(auto future = Client().Chat(), std::runtime_error);
}

UTEST_F(ClientMiddlewaresHooksTest, ExceptionWhenCancelledUnary) {
    EXPECT_CALL(Middleware(0), PreStartCall).Times(1);
    EXPECT_CALL(Middleware(0), PreSendMessage).Times(1);
    EXPECT_CALL(Middleware(0), PostRecvMessage).Times(0);
    EXPECT_CALL(Middleware(0), PostFinish).Times(1);

    Service().SetResponsesBlocked(true);

    {
        sample::ugrpc::GreetingRequest request;
        request.set_name("userver");
        auto future = Client().AsyncSayHello(request);

        engine::current_task::GetCancellationToken().RequestCancel();

        // The destructor of `future` will cancel the RPC and await grpcpp cleanup, then run middlewares.
        // The exception from PostFinish should not lead to a crash.
    }
}

UTEST_F(ClientMiddlewaresHooksTest, BadStatusUnary) {
    EXPECT_CALL(Middleware(0), PreStartCall).Times(1);
    EXPECT_CALL(Middleware(0), PreSendMessage).Times(1);
    EXPECT_CALL(Middleware(0), PostRecvMessage).Times(0);  // skipped, because no response message
    EXPECT_CALL(Middleware(0), PostFinish).Times(1);

    Service().StartFailingWithStatus(grpc::StatusCode::INVALID_ARGUMENT);

    sample::ugrpc::GreetingRequest request;
    request.set_name("userver");
    UEXPECT_THROW(auto response = Client().SayHello(request), ugrpc::client::InvalidArgumentError);
}

UTEST_F(ClientMiddlewaresHooksTest, BadStatusClientStreaming) {
    EXPECT_CALL(Middleware(0), PreStartCall).Times(1);
    EXPECT_CALL(Middleware(0), PreSendMessage).Times(1);
    EXPECT_CALL(Middleware(0), PostRecvMessage).Times(0);  // skipped, because no response message
    EXPECT_CALL(Middleware(0), PostFinish).Times(1);

    Service().StartFailingWithStatus(grpc::StatusCode::INVALID_ARGUMENT);

    sample::ugrpc::StreamGreetingRequest request;
    request.set_name("userver");
    auto stream = Client().WriteMany();

    UASSERT_NO_THROW(stream.WriteAndCheck(request));

    UEXPECT_THROW(auto response = stream.Finish(), ugrpc::client::InvalidArgumentError);
}

UTEST_F(ClientMiddlewaresHooksTest, BadStatusServerStreaming) {
    EXPECT_CALL(Middleware(0), PreStartCall).Times(1);
    EXPECT_CALL(Middleware(0), PreSendMessage).Times(1);
    EXPECT_CALL(Middleware(0), PostRecvMessage).Times(1);  // Second call is skipped, because no response message
    EXPECT_CALL(Middleware(0), PostFinish).Times(1);

    // Fail after first Write (on server side)
    Service().StartFailingWithStatus(grpc::StatusCode::INVALID_ARGUMENT);

    sample::ugrpc::StreamGreetingRequest request;
    request.set_name("userver");
    request.set_number(3);
    sample::ugrpc::StreamGreetingResponse response;

    auto stream = Client().ReadMany(request);

    EXPECT_TRUE(stream.Read(response));

    UEXPECT_THROW([[maybe_unused]] auto ok = stream.Read(response), ugrpc::client::InvalidArgumentError);
}

UTEST_F(ClientMiddlewaresHooksTest, BadStatusBidirectionalStreaming) {
    EXPECT_CALL(Middleware(0), PreStartCall).Times(1);
    EXPECT_CALL(Middleware(0), PreSendMessage).Times(2);
    EXPECT_CALL(Middleware(0), PostRecvMessage).Times(1);  // Second call is skipped, because no response message
    EXPECT_CALL(Middleware(0), PostFinish).Times(0);       // Not called, because no status

    // Fail after first Write (on server side)
    Service().StartFailingWithStatus(grpc::StatusCode::INVALID_ARGUMENT);

    sample::ugrpc::StreamGreetingRequest request;
    request.set_name("userver");
    request.set_number(3);
    sample::ugrpc::StreamGreetingResponse response;

    auto stream = Client().Chat();

    // NOLINTNEXTLINE(clang-analyzer-optin.cplusplus.UninitializedObject)
    UEXPECT_NO_THROW(stream.WriteAndCheck(request));

    EXPECT_TRUE(stream.Read(response));

    // NOLINTNEXTLINE(clang-analyzer-optin.cplusplus.UninitializedObject)
    UEXPECT_THROW(stream.WriteAndCheck(request), ugrpc::client::RpcInterruptedError);
}

USERVER_NAMESPACE_END
