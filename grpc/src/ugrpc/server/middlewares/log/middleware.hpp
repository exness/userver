#pragma once

#include <cstddef>
#include <optional>

#include <userver/logging/level.hpp>

#include <userver/ugrpc/server/middlewares/base.hpp>

USERVER_NAMESPACE_BEGIN

namespace ugrpc::server::middlewares::log {

struct Settings final {
    std::optional<logging::Level> local_log_level{};
    logging::Level msg_log_level{logging::Level::kDebug};
    std::size_t max_msg_size{512};
    bool trim_secrets{true};
};

class Middleware final : public MiddlewareBase {
public:
    explicit Middleware(const Settings& settings);

    void Handle(MiddlewareCallContext& context) const override;

    void CallRequestHook(const MiddlewareCallContext& context, google::protobuf::Message& request) override;

    void CallResponseHook(const MiddlewareCallContext& context, google::protobuf::Message& response) override;

private:
    Settings settings_;
};

}  // namespace ugrpc::server::middlewares::log

USERVER_NAMESPACE_END
