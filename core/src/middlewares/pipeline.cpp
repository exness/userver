#include <userver/middlewares/pipeline.hpp>

#include <fmt/format.h>

#include <userver/components/component_config.hpp>
#include <userver/components/component_context.hpp>
#include <userver/utils/assert.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

#include <userver/middlewares/groups.hpp>
#include <userver/middlewares/pipeline.hpp>
#include <userver/middlewares/runner.hpp>
#include "impl/middlewares_graph.hpp"

USERVER_NAMESPACE_BEGIN

namespace middlewares {

namespace {

impl::Dependencies MakeDependencies(
    const components::ComponentConfig& config,
    const components::ComponentContext& context,
    impl::BasePipelineConfig&& base_config
) {
    impl::BasePipelineConfig userver_deps{base_config};
    auto pipeline_config = config.As<impl::MiddlewarePipelineConfig>();
    pipeline_config.middlewares.merge(userver_deps.middlewares);
    impl::Dependencies dependencies{};
    dependencies.reserve(pipeline_config.middlewares.size());
    for (const auto& [mname, conf] : pipeline_config.middlewares) {
        const auto* middleware = context.FindComponentOptional<impl::WithMiddlewareDependencyComponentBase>(mname);
        if (middleware) {
            auto dep = middleware->GetMiddlewareDependency(utils::impl::InternalTag{});
            dep.enabled = conf.enabled;
            dependencies.emplace(mname, std::move(dep));
        } else {
            UINVARIANT(
                base_config.middlewares.count(mname) != 0,
                fmt::format("The User middleware '{}' is not registered in the component system", mname)
            );
        }
    }
    return dependencies;
}

}  // namespace

namespace impl {

MiddlewarePipeline::MiddlewarePipeline(Dependencies&& deps) : deps_(deps), pipeline_(BuildPipeline(std::move(deps))) {}

std::vector<std::string> MiddlewarePipeline::GetPerServiceMiddlewares(const impl::MiddlewareRunnerConfig& config
) const {
    std::vector<std::string> res{};
    const auto& per_service_middlewares = config.middlewares;
    for (const auto& [name, enabled] : pipeline_) {
        if (const auto it = per_service_middlewares.find(name); it != per_service_middlewares.end()) {
            // Per-service enabled is high priority
            if (it->second.As<BaseMiddlewareConfig>().enabled) {
                res.push_back(name);
            }
            continue;
        }
        if (!enabled || config.disable_all) {
            continue;
        }
        if (config.disable_user_group) {
            const auto it = deps_.find(name);
            UINVARIANT(it != deps_.end(), fmt::format("Middleware `{}` does not exist", name));
            if (it->second.group == "user") {
                continue;
            }
        }
        res.push_back(name);
    }
    return res;
}

AnyMiddlewarePipelineComponent::AnyMiddlewarePipelineComponent(
    const components::ComponentConfig& config,
    const components::ComponentContext& context,
    impl::BasePipelineConfig&& base_config
)
    : components::ComponentBase(config, context),
      pipeline_(MakeDependencies(config, context, std::move(base_config))) {}

const impl::MiddlewarePipeline& AnyMiddlewarePipelineComponent::GetPipeline() const { return pipeline_; }

yaml_config::Schema AnyMiddlewarePipelineComponent::GetStaticConfigSchema() {
    return yaml_config::MergeSchemas<components::ComponentBase>(R"(
type: object
description: base class for all the gRPC service components
additionalProperties: false
properties:
    middlewares:
        type: object
        description: middlewares names and configs to use
        additionalProperties:
            type: object
            description: a middleware config
            additionalProperties: false
            properties:
                enabled:
                    type: boolean
                    description: enable middleware in the list
        properties: {}
)");
}

}  // namespace impl

////////////////////////////////////////////////////////////////////////////////////////////////////////

MiddlewareDependencyBuilder MiddlewareDependencyBuilder::Before(std::string_view before, DependencyType type) && {
    dep_.befores.push_back(impl::Connect{std::string{before}, type});
    return MiddlewareDependencyBuilder(std::move(*this));
}

MiddlewareDependencyBuilder MiddlewareDependencyBuilder::After(std::string_view after, DependencyType type) && {
    dep_.afters.push_back(impl::Connect{std::string{after}, type});
    return MiddlewareDependencyBuilder(std::move(*this));
}

impl::MiddlewareDependency MiddlewareDependencyBuilder::Extract(std::string_view middleware_name) && {
    dep_.middleware_name = std::string{middleware_name};
    return std::move(dep_);
}

}  // namespace middlewares

USERVER_NAMESPACE_END
