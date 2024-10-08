#pragma once

/// @file userver/ydb/credentials.hpp
/// @brief @copybrief ydb::CredentialsProviderComponent

#include <memory>
#include <string>

#include <userver/components/component_base.hpp>
#include <userver/yaml_config/fwd.hpp>

namespace NYdb {
class ICredentialsProviderFactory;
}  // namespace NYdb

USERVER_NAMESPACE_BEGIN

namespace ydb {

class CredentialsProviderComponent : public components::ComponentBase {
 public:
  using components::ComponentBase::ComponentBase;

  virtual std::shared_ptr<NYdb::ICredentialsProviderFactory>
  CreateCredentialsProviderFactory(
      const yaml_config::YamlConfig& credentials) const = 0;
};

}  // namespace ydb

USERVER_NAMESPACE_END
