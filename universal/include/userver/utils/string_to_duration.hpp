#pragma once

/// @file userver/utils/string_to_duration.hpp
/// @brief @copybrief utils::StringToDuration
/// @ingroup userver_universal

#include <chrono>
#include <string_view>

USERVER_NAMESPACE_BEGIN

namespace utils {

/// @brief Converts strings like "10s", "5d", "1h" to durations
///
/// Understands the following suffixes:
///   s   - seconds
///   ms  - milliseconds
///   m   - minutes
///   h   - hours
///   d   - days
std::chrono::milliseconds StringToDuration(std::string_view data);

}  // namespace utils

USERVER_NAMESPACE_END
