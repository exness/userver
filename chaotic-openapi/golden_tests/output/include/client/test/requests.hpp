/* THIS FILE IS AUTOGENERATED, DON'T EDIT! */
#pragma once

#include <string>

#include <userver/clients/http/request.hpp>

namespace clients::test {

namespace testme_post {

using Body = int;

struct Request {
    int number;

    Body body;
};

void SerializeRequest(const Request& request, USERVER_NAMESPACE::clients::http::Request& http_request);
}  // namespace testme_post

}  // namespace clients::test
