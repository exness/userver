#include <userver/utest/utest.hpp>

#include <userver/ugrpc/client/exceptions.hpp>

#include <tests/unit_test_client.usrv.pb.hpp>
#include <tests/unit_test_service.usrv.pb.hpp>
#include <userver/ugrpc/tests/service_fixtures.hpp>

USERVER_NAMESPACE_BEGIN

namespace {

ugrpc::client::CallOptions CallOptionsWithTimeout() {
    ugrpc::client::CallOptions call_options;
    call_options.SetTimeout(utest::kMaxTestWaitTime);
    return call_options;
}

}  // namespace

class GrpcServerAllUnimplementedTest : public ugrpc::tests::ServiceFixtureBase {
protected:
    GrpcServerAllUnimplementedTest() { StartServer(); }

    ~GrpcServerAllUnimplementedTest() override { StopServer(); }
};

UTEST_F(GrpcServerAllUnimplementedTest, Unimplemented) {
    auto client = MakeClient<sample::ugrpc::UnitTestServiceClient>();
    sample::ugrpc::GreetingRequest out;
    out.set_name("userver");
    UEXPECT_THROW(client.SayHello(out, CallOptionsWithTimeout()), ugrpc::client::UnimplementedError);
}

class ChatOnlyService final : public sample::ugrpc::UnitTestServiceBase {
public:
    ChatResult Chat(CallContext& /*context*/, ChatReaderWriter& /*stream*/) override { return grpc::Status::OK; }
};

using GrpcServerSomeUnimplementedTest = ugrpc::tests::ServiceFixture<ChatOnlyService>;

using GrpcServerSomeUnimplementedDeathTest = GrpcServerSomeUnimplementedTest;

UTEST_F(GrpcServerSomeUnimplementedTest, Implemented) {
    auto client = MakeClient<sample::ugrpc::UnitTestServiceClient>();
    auto call = client.Chat(CallOptionsWithTimeout());
    EXPECT_TRUE(call.WritesDone());
    sample::ugrpc::StreamGreetingResponse response;
    EXPECT_FALSE(call.Read(response));
}

UTEST_F_DEATH(GrpcServerSomeUnimplementedDeathTest, Unimplemented) {
    auto client = MakeClient<sample::ugrpc::UnitTestServiceClient>();
    sample::ugrpc::GreetingRequest out;
    out.set_name("userver");
#ifdef NDEBUG
    UEXPECT_THROW(client.SayHello(out, CallOptionsWithTimeout()), ugrpc::client::UnimplementedError);
#else
    UEXPECT_DEATH(client.SayHello(out, CallOptionsWithTimeout()), "Called not implemented");
#endif
}

USERVER_NAMESPACE_END
