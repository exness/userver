#pragma once

#include <userver/ugrpc/server/middlewares/base.hpp>
#include <userver/ugrpc/server/middlewares/congestion_control/component.hpp>

USERVER_NAMESPACE_BEGIN

namespace ugrpc::server::middlewares::deadline_propagation {

class Middleware final : public MiddlewareBase {
public:
    /// @ingroup userver_component_names
    /// @brief The default name of
    // ugrpc::server::middlewares::deadline_propagation::Component
    static inline constexpr std::string_view kName = "grpc-server-deadline-propagation";

    /// @brief dependency of this middleware
    static inline const auto kDependency =
        USERVER_NAMESPACE::middlewares::MiddlewareDependencyBuilder()
            .InGroup<USERVER_NAMESPACE::middlewares::groups::Core>()
            .After<congestion_control::Component>(USERVER_NAMESPACE::middlewares::DependencyType::kWeak);

    void Handle(MiddlewareCallContext& context) const override;
};

}  // namespace ugrpc::server::middlewares::deadline_propagation

USERVER_NAMESPACE_END
