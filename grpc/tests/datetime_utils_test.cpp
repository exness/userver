#include <userver/ugrpc/datetime_utils.hpp>

#include <chrono>
#include <version>

#include <google/protobuf/timestamp.pb.h>
#include <google/protobuf/util/message_differencer.h>
#include <google/type/date.pb.h>

#include <gtest/gtest.h>

#include <userver/formats/json/value_builder.hpp>
#include <userver/utils/mock_now.hpp>

USERVER_NAMESPACE_BEGIN

namespace {

constexpr auto GrpcCompare = google::protobuf::util::MessageDifferencer::Equals;

constexpr std::chrono::system_clock::time_point kTimePoint(std::chrono::seconds(5) + std::chrono::microseconds(123));
const google::protobuf::Timestamp kTimestamp = []() {
    google::protobuf::Timestamp ts;
    ts.set_seconds(5);
    ts.set_nanos(123000);
    return ts;
}();
const formats::json::Value kTsJson = formats::json::ValueBuilder("1970-01-01T00:00:05.000123+00:00").ExtractValue();

constexpr std::chrono::high_resolution_clock::time_point kHighResolutionTimePoint(
    std::chrono::seconds(5) + std::chrono::nanoseconds(123)
);
const google::protobuf::Timestamp kHighResolutionTimestamp = []() {
    google::protobuf::Timestamp ts;
    ts.set_seconds(5);
    ts.set_nanos(123);
    return ts;
}();

}  // namespace

TEST(DatetimeUtils, SystemTimePointToGrpcTimestamp) {
    EXPECT_TRUE(GrpcCompare(kTimestamp, ugrpc::ToGrpcTimestamp(kTimePoint)));
}

TEST(DatetimeUtils, HightResolutionTimePointToGrpcTimestamp) {
    EXPECT_TRUE(GrpcCompare(kHighResolutionTimestamp, ugrpc::ToGrpcTimestamp(kHighResolutionTimePoint)));
}

TEST(DatetimeUtils, TimestampToSystemClock) {
    EXPECT_EQ(kTimePoint, ugrpc::ToTimePoint<std::chrono::system_clock>(kTimestamp));
}

TEST(DatetimeUtils, TimestampToHighResolutionClock) {
    EXPECT_EQ(
        kHighResolutionTimePoint, ugrpc::ToTimePoint<std::chrono::high_resolution_clock>(kHighResolutionTimestamp)
    );
}

TEST(DatetimeUtils, TimestampToSystemClockDefaultTemplate) { EXPECT_EQ(kTimePoint, ugrpc::ToTimePoint(kTimestamp)); }

TEST(DatetimeUtils, NowTimestamp) {
    utils::datetime::MockNowSet(kTimePoint);
    EXPECT_TRUE(GrpcCompare(kTimestamp, ugrpc::NowTimestamp()));
}

TEST(DatetimeUtils, TimestampJsonParse) {
    EXPECT_TRUE(GrpcCompare(kTimestamp, kTsJson.As<google::protobuf::Timestamp>()));
}

TEST(DatetimeUtils, TimestampJsonSerialize) {
    EXPECT_EQ(formats::json::ValueBuilder(kTimestamp).ExtractValue(), kTsJson);
}

namespace {

#if __cpp_lib_chrono >= 201907L
constexpr std::chrono::year_month_day
    kYearMonthDay(std::chrono::year(2025), std::chrono::month(4), std::chrono::day(10));
#endif

const google::type::Date kDate = []() {
    google::type::Date date;
    date.set_year(2025);
    date.set_month(4);
    date.set_day(10);
    return date;
}();
const utils::datetime::Date kUtilsDate(2025, 4, 10);
constexpr std::chrono::system_clock::time_point kDateTimePoint(std::chrono::seconds(1744287725));
constexpr std::chrono::system_clock::time_point kDateTimePointRounded(std::chrono::seconds(1744243200));
const formats::json::Value kDateJson = formats::json::ValueBuilder("2025-04-10").ExtractValue();

}  // namespace

#if __cpp_lib_chrono >= 201907L

TEST(DatetimeUtils, ToGrpcDateFromYearMonthDay) { EXPECT_TRUE(GrpcCompare(kDate, ugrpc::ToGrpcDate(kYearMonthDay))); }

TEST(DatetimeUtils, ToYearMonthDay) { EXPECT_EQ(kYearMonthDay, ugrpc::ToYearMonthDay(kDate)); }

#endif

TEST(DatetimeUtils, ToGrpcDateFromUtilsDate) { EXPECT_TRUE(GrpcCompare(kDate, ugrpc::ToGrpcDate(kUtilsDate))); }

TEST(DatetimeUtils, ToUtilsDate) { EXPECT_EQ(kUtilsDate, ugrpc::ToUtilsDate(kDate)); }

TEST(DatetimeUtils, ToGrpcDateFromTimePoint) { EXPECT_TRUE(GrpcCompare(kDate, ugrpc::ToGrpcDate(kDateTimePoint))); }

TEST(DatetimeUtils, DateToSystemClock) {
    EXPECT_EQ(kDateTimePointRounded, ugrpc::ToTimePoint<std::chrono::system_clock>(kDate));
}

TEST(DatetimeUtils, DateToSystemClockDefaultTemplate) { EXPECT_EQ(kDateTimePointRounded, ugrpc::ToTimePoint(kDate)); }

TEST(DatetimeUtils, NowDate) {
    utils::datetime::MockNowSet(kDateTimePoint);
    EXPECT_TRUE(GrpcCompare(kDate, ugrpc::NowDate()));
}

TEST(DatetimeUtils, DateJsonParse) { EXPECT_TRUE(GrpcCompare(kDate, kDateJson.As<google::type::Date>())); }

TEST(DatetimeUtils, DateJsonSerialize) { EXPECT_EQ(formats::json::ValueBuilder(kDate).ExtractValue(), kDateJson); }

namespace {

const google::protobuf::Duration kGrpcDuration = []() {
    google::protobuf::Duration duration;
    duration.set_seconds(123);
    duration.set_nanos(5678);
    return duration;
}();
constexpr std::chrono::duration kDurationSeconds = std::chrono::seconds(123);
constexpr std::chrono::duration kDuration = kDurationSeconds + std::chrono::nanoseconds(5678);

}  // namespace

TEST(DatetimeUtils, ToDurationSeconds) {
    EXPECT_EQ(kDurationSeconds, ugrpc::ToDuration<std::chrono::seconds>(kGrpcDuration));
}

TEST(DatetimeUtils, ToDurationNanoseconds) {
    EXPECT_EQ(kDuration, ugrpc::ToDuration<std::chrono::nanoseconds>(kGrpcDuration));
}

TEST(DatetimeUtils, ToDurationDefaultTemplate) { EXPECT_EQ(kDuration, ugrpc::ToDuration(kGrpcDuration)); }

TEST(DatetimeUtils, ToGrpcDuration) { EXPECT_TRUE(GrpcCompare(kGrpcDuration, ugrpc::ToGrpcDuration(kDuration))); }

USERVER_NAMESPACE_END
