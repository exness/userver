#include <userver/http/http_version.hpp>

#include <userver/utils/trivial_map.hpp>
#include <userver/yaml_config/yaml_config.hpp>

USERVER_NAMESPACE_BEGIN

namespace http {

static constexpr utils::TrivialBiMap kMap([](auto selector) {
    return selector()
        .Case(HttpVersion::kDefault, "default")
        .Case(HttpVersion::k10, "1.0")
        .Case(HttpVersion::k11, "1.1")
        .Case(HttpVersion::k2, "2")
        .Case(HttpVersion::k2Tls, "2tls")
        .Case(HttpVersion::k2PriorKnowledge, "2-prior");
});

std::string_view ToString(HttpVersion version) { return utils::impl::EnumToStringView(version, kMap); }

HttpVersion HttpVersionFromString(std::string_view version) {
    const auto result = kMap.TryFindBySecond(version);
    if (result.has_value()) {
        return *result;
    }

    throw std::runtime_error(
        fmt::format("Invalid enum value ({}) for HttpVersion. Allowed values: {}", version, kMap.DescribeSecond())
    );
}

HttpVersion Parse(const yaml_config::YamlConfig& value, formats::parse::To<HttpVersion>) {
    return utils::ParseFromValueString(value, kMap);
}

}  // namespace http

USERVER_NAMESPACE_END
