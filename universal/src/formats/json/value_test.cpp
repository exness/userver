#include <gtest/gtest.h>

#include <userver/formats/json/exception.hpp>
#include <userver/formats/json/serialize.hpp>
#include <userver/formats/json/serialize_container.hpp>
#include <userver/formats/json/value.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/utest/literals.hpp>

#include <formats/common/value_test.hpp>

USERVER_NAMESPACE_BEGIN

template <>
struct Parsing<formats::json::Value> : public ::testing::Test {
    constexpr static auto FromString = formats::json::FromString;
    using ParseException = formats::json::Value::ParseException;
};

INSTANTIATE_TYPED_TEST_SUITE_P(FormatsJson, Parsing, formats::json::Value);

TEST(FormatsJson, ParsingInvalidRootType) {
    using formats::json::FromString;
    using ParseException = formats::json::Value::ParseException;

    ASSERT_NO_THROW(FromString("{}"));
    ASSERT_TRUE(FromString("{}").IsObject());
    ASSERT_NO_THROW(FromString("[]"));
    ASSERT_TRUE(FromString("[]").IsArray());

    ASSERT_NO_THROW(FromString("null"));
    ASSERT_NO_THROW(FromString("true"));
    ASSERT_NO_THROW(FromString("false"));
    ASSERT_NO_THROW(FromString("0"));
    ASSERT_NO_THROW(FromString("1.5"));
    ASSERT_NO_THROW(FromString("-1.2e-0123"));
    ASSERT_NO_THROW(FromString("-1.2E34"));
    ASSERT_NO_THROW(FromString("1.2E+34"));
    ASSERT_NO_THROW(FromString(R"("string")"));
    ASSERT_NO_THROW(FromString(R"("")"));

    ASSERT_THROW(FromString("NULL"), ParseException);
    ASSERT_THROW(FromString("True"), ParseException);
    ASSERT_THROW(FromString("00"), ParseException);
    ASSERT_THROW(FromString(""), ParseException);
    ASSERT_THROW(FromString("inf"), ParseException);
    ASSERT_THROW(FromString("#INF"), ParseException);
    ASSERT_THROW(FromString("nan"), ParseException);
    ASSERT_THROW(FromString("NaN"), ParseException);

    ASSERT_THROW(FromString(R"({"field": 'string'})"), ParseException);
    ASSERT_THROW(FromString("{}{}"), ParseException);
}

void TestLargeDoubleValueAsInt64(const std::string& json_str, int64_t expected_value, bool expect_ok) {
    auto json = formats::json::FromString(json_str);
    int64_t parsed_value = 0;
    if (expect_ok) {
        ASSERT_NO_THROW(parsed_value = json["value"].As<int64_t>()) << "json: " << json_str;
        EXPECT_EQ(parsed_value, expected_value)
            << "json: " << json_str << ", parsed double: " << json["value"].As<double>();
    } else {
        ASSERT_THROW(parsed_value = json["value"].As<int64_t>(), formats::json::TypeMismatchException)
            << "json: " << json_str;
    }
}

class TestIncorrectValueException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void CheckExactValues(int bits) {
    const int64_t start = (1L << bits);
    for (int add = -20; add <= 0; ++add) {
        const int64_t value = start + add;
        const std::string json_str = R"({"value": )" + std::to_string(value) + ".0}";
        auto json = formats::json::FromString(json_str);
        auto dval = json["value"].As<double>();
        auto ival = static_cast<int64_t>(dval);
        if (ival != value) throw TestIncorrectValueException("test");
    }
}

TEST(FormatsJson, LargeDoubleValueAsInt64) {
    const int kMaxCorrectBits = 53;

    for (int bits = kMaxCorrectBits; bits >= kMaxCorrectBits - 5; --bits) {
        const int64_t start = (1L << bits);
        const int max_add = bits == kMaxCorrectBits ? -1 : 20;
        for (int add = max_add; add >= -20; --add) {
            const int64_t value = start + add;
            std::string json_str = R"({"value": )" + std::to_string(value) + ".0}";
            TestLargeDoubleValueAsInt64(json_str, value, true);
            json_str = R"({"value": )" + std::to_string(-value) + ".0}";
            TestLargeDoubleValueAsInt64(json_str, -value, true);
        }
    }

    ASSERT_THROW(CheckExactValues(kMaxCorrectBits + 1), TestIncorrectValueException);

    // 2 ** 53 == 9007199254740992
    TestLargeDoubleValueAsInt64(R"({"value": 9007199254740992.0})", 9007199254740992, false);
    TestLargeDoubleValueAsInt64(R"({"value": 9007199254740993.0})", 9007199254740993, false);
    TestLargeDoubleValueAsInt64(R"({"value": -9007199254740992.0})", -9007199254740992, false);
    TestLargeDoubleValueAsInt64(R"({"value": -9007199254740993.0})", -9007199254740993, false);
}

TEST(FormatsJson, ParseNanInf) {
    using formats::json::FromString;
    using ParseException = formats::json::Value::ParseException;

    ASSERT_THROW(FromString(R"({"field": NaN})"), ParseException);
    ASSERT_THROW(FromString(R"({"field": Inf})"), ParseException);
    ASSERT_THROW(FromString(R"({"field": -Inf})"), ParseException);
}

TEST(FormatsJson, NulString) {
    std::string i_contain_nuls = "test";
    i_contain_nuls += '\x00';
    i_contain_nuls += "test";

    auto s = formats::json::ValueBuilder(i_contain_nuls).ExtractValue().As<std::string>();
    ASSERT_EQ(i_contain_nuls, s);
}

TEST(FormatsJson, NullAsDefaulted) {
    using formats::json::FromString;
    auto json = FromString(R"~({"nulled": null})~");

    EXPECT_EQ(json["nulled"].As<int>({}), 0);
    EXPECT_EQ(json["nulled"].As<std::vector<int>>({}), std::vector<int>{});

    EXPECT_EQ(json["nulled"].As<int>(42), 42);

    const std::vector<int> value{4, 2};
    EXPECT_EQ(json["nulled"].As<std::vector<int>>(value), value);
}

TEST(FormatsJson, ExampleUsage) {
    /// [Sample formats::json::Value usage]
    // #include <userver/formats/json.hpp>

    const formats::json::Value json = formats::json::FromString(R"({
    "key1": 1,
    "key2": {"key3":"val"}
  })");

    const auto key1 = json["key1"].As<int>();
    ASSERT_EQ(key1, 1);

    const auto key3 = json["key2"]["key3"].As<std::string>();
    ASSERT_EQ(key3, "val");
    /// [Sample formats::json::Value usage]
}

/// [Sample formats::json::Value::As<T>() usage]
namespace my_namespace {

struct MyKeyValue {
    std::string field1;
    int field2;
};
//  The function must be declared in the namespace of your type
MyKeyValue Parse(const formats::json::Value& json, formats::parse::To<MyKeyValue>) {
    return MyKeyValue{
        json["field1"].As<std::string>(""),
        json["field2"].As<int>(1),  // return `1` if "field2" is missing
    };
}

TEST(FormatsJson, ExampleUsageMyStruct) {
    const formats::json::Value json = formats::json::FromString(R"({
    "my_value": {
        "field1": "one",
        "field2": 1
    }
  })");
    auto data = json["my_value"].As<MyKeyValue>();
    EXPECT_EQ(data.field1, "one");
    EXPECT_EQ(data.field2, 1);
}
}  // namespace my_namespace
/// [Sample formats::json::Value::As<T>() usage]

TEST(FormatsJson, UserDefinedLiterals) {
    using ValueBuilder = formats::json::ValueBuilder;
    ValueBuilder builder{formats::common::Type::kObject};
    builder["test"] = 3;
    EXPECT_EQ(
        builder.ExtractValue(),
        R"json(
    {"test" : 3}
    )json"_json
    );
}

TEST(FormatsJson, DropRootPath) {
    static constexpr std::string_view kJson = R"({
    "foo": {
      "bar": "baz"
    }
  })";
    formats::json::Value child;

    {
        const auto root = formats::json::FromString(kJson);
        EXPECT_EQ(root.GetPath(), "/");

        auto foo = root["foo"];
        EXPECT_EQ(foo.GetPath(), "foo");
        foo.DropRootPath();
        EXPECT_EQ(foo.GetPath(), "/");

        for (auto [_, value] : Items(foo)) {
            EXPECT_EQ(value.GetPath(), "bar");
            child = value;
        }
    }

    EXPECT_EQ(child.GetPath(), "bar");
}

TEST(FormatsJson, ExceptionMessages) {
    const formats::json::Value json = formats::json::FromString(R"({
    "foo": {
      "bar": "baz"
    }
  })");

    try {
        auto _ [[maybe_unused]] = json["foo"]["bar"].As<int64_t>();
    } catch (const formats::json::TypeMismatchException& ex) {
        EXPECT_EQ(ex.GetPath(), "foo.bar");
        EXPECT_EQ(ex.GetMessageWithoutPath(), "Wrong type. Expected: intValue, actual: stringValue");
    }
}

TEST(FormatsJson, IsUint) {
    const auto json = formats::json::FromString(R"({
      "uint": 42,
      "negative": -1,
      "string": "42",
      "bool": true,
      "uint64": 5294967295,
      "null": null
    })");

    EXPECT_TRUE(json["uint"].IsUInt());

    EXPECT_FALSE(json["negative"].IsUInt());
    EXPECT_FALSE(json["string"].IsUInt());
    EXPECT_FALSE(json["bool"].IsUInt());
    EXPECT_FALSE(json["uint64"].IsUInt());
    EXPECT_FALSE(json["null"].IsUInt());

    const auto json_double = formats::json::FromString(R"({
      "valid": 123.0,
      "invalid": 123.45
    })");
    EXPECT_TRUE(json_double["valid"].IsUInt());
    EXPECT_FALSE(json_double["invalid"].IsUInt());

    const auto json_bounds = formats::json::FromString(R"({
      "max": 4294967295,
      "overflow": 4294967296,
      "min": 0,
      "below_min": -1
    })");
    EXPECT_TRUE(json_bounds["max"].IsUInt());
    EXPECT_FALSE(json_bounds["overflow"].IsUInt());
    EXPECT_TRUE(json_bounds["min"].IsUInt());
    EXPECT_FALSE(json_bounds["below_min"].IsUInt());
}

USERVER_NAMESPACE_END
