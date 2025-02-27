#pragma once

#include <string>
#include <unordered_map>
#include <variant>

#include <userver/formats/json/value.hpp>
#include <userver/formats/parse/to.hpp>
#include <userver/formats/serialize/to.hpp>
#include <userver/utils/algo.hpp>
#include <userver/utils/constexpr_indices.hpp>
#include <userver/utils/overloaded.hpp>
#include <userver/utils/trivial_map.hpp>

USERVER_NAMESPACE_BEGIN

namespace chaotic {

template <typename BuilderFunc>
struct OneOfStringSettings {
    using KeyType = std::string;

    std::string_view property_name;
    utils::TrivialSet<BuilderFunc> mapping;

    constexpr std::optional<std::size_t> GetIndex(std::string_view key) const { return mapping.GetIndex(key); }

    static std::string_view FieldToString(const KeyType& key) { return key; }
};

template <typename BuilderFunc>
OneOfStringSettings(const char*, utils::TrivialSet<BuilderFunc>) -> OneOfStringSettings<BuilderFunc>;

struct OneOfIntegerSettings {
    using KeyType = int64_t;

    std::string_view property_name;
    std::unordered_map<KeyType, std::size_t> mapping;

    std::optional<std::size_t> GetIndex(KeyType key) const { return utils::FindOptional(mapping, key); }

    static std::string FieldToString(const KeyType& key) { return std::to_string(key); }
};

template <const auto* Settings, typename... T>
struct OneOfWithDiscriminator {
    const std::variant<formats::common::ParseType<formats::json::Value, T>...>& value;
};

template <const auto* Settings, typename... T, typename Value>
std::variant<formats::common::ParseType<Value, T>...>
Parse(Value value, formats::parse::To<OneOfWithDiscriminator<Settings, T...>>) {
    using SettingsType = std::decay_t<decltype(*Settings)>;
    const auto field = value[Settings->property_name].template As<typename SettingsType::KeyType>();

    const auto index = Settings->GetIndex(field);
    if (!index.has_value()) {
        throw formats::json::UnknownDiscriminatorException(value.GetPath(), SettingsType::FieldToString(field));
    }

    using Result = std::variant<formats::common::ParseType<formats::json::Value, T>...>;

    Result result;
    utils::WithConstexprIndex<sizeof...(T)>(index.value(), [&](auto index_constant) {
        constexpr auto kIndex = decltype(index_constant)::value;
        result.template emplace<kIndex>(value.template As<std::variant_alternative_t<kIndex, Result>>());
    });
    return result;
}

template <const auto* Settings, typename... T, typename Value>
Value Serialize(const OneOfWithDiscriminator<Settings, T...>& var, formats::serialize::To<Value>) {
    return std::visit(
        USERVER_NAMESPACE::utils::Overloaded{[](const formats::common::ParseType<Value, T>& item) {
            return typename Value::Builder(T{item}).ExtractValue();
        }...},
        var.value
    );
}

}  // namespace chaotic

USERVER_NAMESPACE_END
