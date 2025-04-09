#include <userver/utest/utest.hpp>

#include <userver/clients/http/client.hpp>
#include <userver/utest/assert_macros.hpp>
#include <userver/utest/http_client.hpp>
#include <userver/utest/http_server_mock.hpp>

#include <client/multiple_content_types/responses.hpp>
#include <client/test_object/responses.hpp>

USERVER_NAMESPACE_BEGIN

namespace {

UTEST(Responses, Status200) {
    utest::HttpServerMock http_server([](const utest::HttpServerMock::HttpRequest&) {
        utest::HttpServerMock::HttpResponse r;
        r.response_status = 200;
        r.body = R"({"bar": "bar"})";
        return r;
    });
    auto http_client = utest::CreateHttpClient();
    auto http_response = http_client->CreateRequest().get(http_server.GetBaseUrl() + "/test1").perform();

    auto response = ::clients::test_object::test1_post::ParseResponse(*http_response);
    auto response200 = std::get<::clients::test_object::test1_post::Response200>(response);
    EXPECT_EQ(response200.body.bar, "bar");
}

UTEST(Responses, Status500) {
    utest::HttpServerMock http_server([](const utest::HttpServerMock::HttpRequest&) {
        utest::HttpServerMock::HttpResponse r;
        r.response_status = 500;
        return r;
    });
    auto http_client = utest::CreateHttpClient();
    auto http_response = http_client->CreateRequest().get(http_server.GetBaseUrl() + "/test1").perform();

    UEXPECT_THROW(
        ::clients::test_object::test1_post::ParseResponse(*http_response),
        ::clients::test_object::test1_post::Response500
    );
}

UTEST(ResponsesMultipleContentType, ApplicationJson) {
    utest::HttpServerMock http_server([](const utest::HttpServerMock::HttpRequest&) {
        utest::HttpServerMock::HttpResponse r;
        r.response_status = 200;
        r.body = R"({"bar": "a"})";
        r.headers[std::string{"Content-Type"}] = "application/json";
        return r;
    });
    auto http_client = utest::CreateHttpClient();
    auto http_response = http_client->CreateRequest().get(http_server.GetBaseUrl() + "/test1").perform();

    namespace client = ::clients::multiple_content_types::test1_post;
    EXPECT_EQ(
        std::get<client::Response200BodyApplicationJson>(client::ParseResponse(*http_response).body),
        (client::Response200BodyApplicationJson{"a"})
    );
}

UTEST(ResponsesMultipleContentType, ApplicationOctetStream) {
    utest::HttpServerMock http_server([](const utest::HttpServerMock::HttpRequest&) {
        utest::HttpServerMock::HttpResponse r;
        r.response_status = 200;
        r.body = "blabla";
        r.headers[std::string{"Content-Type"}] = "application/octet-stream";
        return r;
    });
    auto http_client = utest::CreateHttpClient();
    auto http_response = http_client->CreateRequest().get(http_server.GetBaseUrl() + "/test1").perform();

    namespace client = ::clients::multiple_content_types::test1_post;
    EXPECT_EQ(
        std::get<client::Response200ApplicationOctetStream>(client::ParseResponse(*http_response).body),
        (client::Response200ApplicationOctetStream{"blabla"})
    );
}

}  // namespace

USERVER_NAMESPACE_END
