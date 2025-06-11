#pragma once

/// @file
/// @brief Utilities for @c google::protobuf::Timestamp, @c google::type::Date, and @c google::protobuf::Duration types.

#include <chrono>
#include <version>

#include <google/protobuf/duration.pb.h>
#include <google/protobuf/timestamp.pb.h>
#include <google/type/date.pb.h>

#include <userver/formats/json/value.hpp>
#include <userver/formats/parse/to.hpp>
#include <userver/formats/serialize/to.hpp>
#include <userver/utils/datetime/date.hpp>

USERVER_NAMESPACE_BEGIN

namespace ugrpc {

/// @brief Creates @c google::protobuf::Timestamp from @c std::chrono::time_point
template <class Clock, class Duration>
google::protobuf::Timestamp ToGrpcTimestamp(const std::chrono::time_point<Clock, Duration>& system_tp) {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(system_tp.time_since_epoch());
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(system_tp.time_since_epoch() - seconds);

    google::protobuf::Timestamp timestamp;
    timestamp.set_seconds(seconds.count());
    timestamp.set_nanos(nanos.count());
    return timestamp;
}

/// @brief Creates @c std::chrono::time_point from @c google::protobuf::Timestamp
template <class Clock = std::chrono::system_clock>
std::chrono::time_point<Clock> ToTimePoint(const google::protobuf::Timestamp& grpc_ts) {
    const auto duration = std::chrono::duration_cast<typename Clock::duration>(
        std::chrono::seconds(grpc_ts.seconds()) + std::chrono::nanoseconds(grpc_ts.nanos())
    );
    return std::chrono::time_point<Clock>(duration);
}

/// @brief Returns current (possibly, mocked) timestamp as a @c google::protobuf::Timestamp
google::protobuf::Timestamp NowTimestamp();

#if __cpp_lib_chrono >= 201907L

/// @brief Creates @c google::type::Date from @c std::chrono::year_month_day
google::type::Date ToGrpcDate(const std::chrono::year_month_day& system_date);

/// @brief Creates @c std::chrono::year_month_day from @c google::type::Date
std::chrono::year_month_day ToYearMonthDay(const google::type::Date& grpc_date);

#endif

/// @brief Creates @c google::type::Date from @c utils::datetime::Date
google::type::Date ToGrpcDate(const utils::datetime::Date& utils_date);

/// @brief Creates @c utils::datetime::Date from @c google::type::Date
utils::datetime::Date ToUtilsDate(const google::type::Date& grpc_date);

/// @brief Creates @c google::type::Date from @c std::chrono::time_point
template <class Clock, class Duration>
google::type::Date ToGrpcDate(const std::chrono::time_point<Clock, Duration>& system_tp) {
    return ToGrpcDate(utils::datetime::Date(std::chrono::floor<utils::datetime::Date::Days>(system_tp)));
}

/// @brief Creates @c std::chrono::time_point from @c google::type::Date
template <class Clock = std::chrono::system_clock>
std::chrono::time_point<Clock> ToTimePoint(const google::type::Date& grpc_date) {
    return ToUtilsDate(grpc_date).GetSysDays();
}

/// @brief Returns current (possibly, mocked) timestamp as a @c google::type::Date
google::type::Date NowDate();

/// @brief Creates @c std::chrono::duration from @c google::protobuf::Duration
template <class Duration = std::chrono::nanoseconds>
Duration ToDuration(const google::protobuf::Duration& duration) {
    return std::chrono::duration_cast<Duration>(
        std::chrono::seconds(duration.seconds()) + std::chrono::nanoseconds(duration.nanos())
    );
}

/// @brief Creates @c google::protobuf::Duration from @c std::chrono::duration
template <class Rep, class Period>
google::protobuf::Duration ToGrpcDuration(const std::chrono::duration<Rep, Period>& duration) {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(duration - seconds);

    google::protobuf::Duration result;
    result.set_seconds(seconds.count());
    result.set_nanos(nanos.count());
    return result;
}

}  // namespace ugrpc

namespace formats::parse {

google::protobuf::Timestamp Parse(const formats::json::Value& json, formats::parse::To<google::protobuf::Timestamp>);

google::type::Date Parse(const formats::json::Value& json, formats::parse::To<google::type::Date>);

}  // namespace formats::parse

namespace formats::serialize {

formats::json::Value Serialize(const google::protobuf::Timestamp& value, formats::serialize::To<formats::json::Value>);

formats::json::Value Serialize(const google::type::Date& value, formats::serialize::To<formats::json::Value>);

}  // namespace formats::serialize

USERVER_NAMESPACE_END
