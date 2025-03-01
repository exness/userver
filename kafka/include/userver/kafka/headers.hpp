#pragma once

#include <string_view>

#include <userver/utils/null_terminated_view.hpp>
#include <userver/utils/span.hpp>

USERVER_NAMESPACE_BEGIN

namespace kafka {

/// @brief Wrapper for Kafka header.
/// Header `name` must be valid null-terminated string.
/// Header `value` is treated as a binary data by Kafka.
struct HeaderView final {
    utils::NullTerminatedView name;
    std::string_view value;
};

/// @brief Ordered list of HeaderView.
/// Duplicated keys are supported.
/// Keys order is preserved.
using HeaderViews = utils::span<const HeaderView>;

}  // namespace kafka

USERVER_NAMESPACE_END
