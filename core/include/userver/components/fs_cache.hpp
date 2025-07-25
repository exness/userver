#pragma once

/// @file userver/components/fs_cache.hpp
/// @brief @copybrief components::FsCache

#include <userver/components/component_base.hpp>
#include <userver/fs/fs_cache_client.hpp>
#include <userver/yaml_config/fwd.hpp>

USERVER_NAMESPACE_BEGIN

namespace components {

// clang-format off

/// @ingroup userver_components
///
/// @brief Component for storing files in memory
/// ## Static options:
///
/// Name              | Description                                          | Default value
/// ----------------- | ---------------------------------------------------- | -------------
/// dir               | directory to cache files from                        | /var/www
/// update-period     | Update period (0 - fill the cache only at startup)   | 0
/// fs-task-processor | task processor to do filesystem operations           | engine::current_task::GetBlockingTaskProcessor()

// clang-format on

class FsCache final : public components::ComponentBase {
public:
    using Client = fs::FsCacheClient;

    FsCache(const components::ComponentConfig& config, const components::ComponentContext& context);

    static yaml_config::Schema GetStaticConfigSchema();

    const Client& GetClient() const;

private:
    Client client_;
};

template <>
inline constexpr bool kHasValidate<FsCache> = true;

}  // namespace components

USERVER_NAMESPACE_END
