#pragma once

#include <chrono>
#include <optional>

#include <userver/chaotic/openapi/client/config.hpp>

USERVER_NAMESPACE_BEGIN

namespace chaotic::openapi::client {

struct CommandControl {
    std::chrono::milliseconds timeout{};
    int attempts{};
    std::optional<bool> follow_redirects{};

    explicit operator bool() const { return timeout.count() != 0 || attempts != 0 || follow_redirects.has_value(); }
};

}  // namespace chaotic::openapi::client

USERVER_NAMESPACE_END
