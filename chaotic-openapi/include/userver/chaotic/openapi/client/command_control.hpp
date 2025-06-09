#pragma once

#include <chrono>

#include <userver/chaotic/openapi/client/config.hpp>

USERVER_NAMESPACE_BEGIN

namespace chaotic::openapi::client {

struct CommandControl {
    std::chrono::milliseconds timeout{};
    int attempts{};
};

void ApplyConfig(clients::http::Request& request, const CommandControl& cc, const Config& config);

}  // namespace chaotic::openapi::client

USERVER_NAMESPACE_END
