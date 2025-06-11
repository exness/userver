#include <userver/ugrpc/datetime_utils.hpp>

#include <cstdint>

#include <date/date.h>

#include <userver/formats/json/value_builder.hpp>
#include <userver/utils/datetime/date.hpp>
#include <userver/utils/datetime_light.hpp>

USERVER_NAMESPACE_BEGIN

namespace ugrpc {

google::protobuf::Timestamp NowTimestamp() { return ToGrpcTimestamp(utils::datetime::Now()); }

#if __cpp_lib_chrono >= 201907L

google::type::Date ToGrpcDate(const std::chrono::year_month_day& system_date) {
    google::type::Date date;
    date.set_year(static_cast<int>(system_date.year()));
    date.set_month(static_cast<std::uint32_t>(system_date.month()));
    date.set_day(static_cast<std::uint32_t>(system_date.day()));
    return date;
}

std::chrono::year_month_day ToYearMonthDay(const google::type::Date& grpc_date) {
    return {
        std::chrono::year(grpc_date.year()), std::chrono::month(grpc_date.month()), std::chrono::day(grpc_date.day())};
}

#endif

google::type::Date ToGrpcDate(const utils::datetime::Date& utils_date) {
    const date::year_month_day ymd(utils_date.GetSysDays());

    google::type::Date date;
    date.set_year(static_cast<int>(ymd.year()));
    date.set_month(static_cast<std::uint32_t>(ymd.month()));
    date.set_day(static_cast<std::uint32_t>(ymd.day()));
    return date;
}

utils::datetime::Date ToUtilsDate(const google::type::Date& grpc_date) {
    return utils::datetime::Date(grpc_date.year(), grpc_date.month(), grpc_date.day());
}

google::type::Date NowDate() { return ToGrpcDate(utils::datetime::Now()); }

}  // namespace ugrpc

namespace formats::parse {

google::protobuf::Timestamp Parse(const formats::json::Value& json, formats::parse::To<google::protobuf::Timestamp>) {
    return ugrpc::ToGrpcTimestamp(json.As<std::chrono::system_clock::time_point>());
}

google::type::Date Parse(const formats::json::Value& json, formats::parse::To<google::type::Date>) {
    return ugrpc::ToGrpcDate(json.As<utils::datetime::Date>());
}

}  // namespace formats::parse

namespace formats::serialize {

formats::json::Value Serialize(const google::protobuf::Timestamp& value, formats::serialize::To<formats::json::Value>) {
    return formats::json::ValueBuilder(ugrpc::ToTimePoint<std::chrono::system_clock>(value)).ExtractValue();
}

formats::json::Value Serialize(const google::type::Date& value, formats::serialize::To<formats::json::Value>) {
    return formats::json::ValueBuilder(ugrpc::ToUtilsDate(value)).ExtractValue();
}

}  // namespace formats::serialize

USERVER_NAMESPACE_END
