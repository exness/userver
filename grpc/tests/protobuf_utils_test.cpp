#include <userver/utest/utest.hpp>

#include <userver/ugrpc/impl/to_string.hpp>

#include <ugrpc/impl/protobuf_utils.hpp>

#include <tests/logging.pb.h>
#include <tests/messages.pb.h>

using namespace std::chrono_literals;

USERVER_NAMESPACE_BEGIN

namespace {

sample::ugrpc::LoggingMessage ConstructComplexMessage() {
    sample::ugrpc::LoggingMessage message;

    message.set_id("test-id");

    for (int i = 0; i < 10; ++i) {
        *message.add_names() = "test-name-" + std::to_string(i);
    }

    for (int i = 0; i < 10; ++i) {
        auto* item = message.add_items();
        item->set_index(i);
        item->set_value("test-value-" + std::to_string(i));
    }

    for (int i = 0; i < 10; ++i) {
        (*message.mutable_properties())["test-property-name-" + std::to_string(i)] =
            "test-property-" + std::to_string(i);
    }

    return message;
}

}  // namespace

UTEST(ToLimitedString, Fit) {
    std::size_t kLimit = 20;
    sample::ugrpc::GreetingResponse message;
    message.set_name("1234567890");
    const auto out = ugrpc::impl::ToLimitedString(message, kLimit);
    EXPECT_EQ(out, "name: \"1234567890\"\n") << out;
}

UTEST(ToLimitedString, Limited) {
    std::size_t kLimit = 10;
    sample::ugrpc::GreetingResponse message;
    message.set_name("1234567890");
    const auto out = ugrpc::impl::ToLimitedString(message, kLimit);
    EXPECT_EQ(out, "name: \"123") << out;
}

UTEST(ToLimitedString, Complex) {
    std::size_t kLimit = 512;
    const auto message = ConstructComplexMessage();
    const auto expected = ugrpc::impl::ToString(message.Utf8DebugString().substr(0, kLimit));
    ASSERT_EQ(expected, ugrpc::impl::ToLimitedString(message, kLimit));
}

USERVER_NAMESPACE_END
