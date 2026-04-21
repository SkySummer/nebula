#include "nebula/common/json.hpp"

#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "nebula_tests/test_support.hpp"

namespace {

using nebula::common::dump_json;
using nebula::common::JsonArray;
using nebula::common::JsonObject;
using nebula::common::JsonParseResult;
using nebula::common::JsonValue;
using nebula::common::parse_json;
using nebula::testsupport::expect_equal;
using nebula::testsupport::expect_true;

constexpr std::int64_t kInt64Max = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kInt64Min = std::numeric_limits<std::int64_t>::min();
constexpr std::size_t kJsonNestingDepthLimit = 256U;

JsonParseResult parse_or_fail(std::string_view text) {
    const JsonParseResult result = parse_json(text);
    expect_true(result.ok, std::format("expected parse success: {}", text));
    return result;
}

void expect_parse_error(std::string_view text, std::string_view expected_error) {
    const JsonParseResult result = parse_json(text);
    expect_true(!result.ok, "expected parse failure");
    expect_equal(result.error, std::string(expected_error), "error code should match");
}

std::string build_nested_array_json(std::size_t depth) {
    std::string json;
    json.reserve((depth * 2U) + 1U);
    json.append(depth, '[');
    json.push_back('0');
    json.append(depth, ']');
    return json;
}

std::string build_nested_object_array_json(std::size_t depth) {
    std::string json;
    json.reserve((depth * 6U) + 1U);
    for (std::size_t idx = 0; idx < depth; ++idx) {
        if ((idx % 2U) == 0U) {
            json.append(R"({"a":)");
        } else {
            json.push_back('[');
        }
    }

    json.push_back('0');

    for (std::size_t idx = depth; idx > 0U; --idx) {
        if (((idx - 1U) % 2U) == 0U) {
            json.push_back('}');
        } else {
            json.push_back(']');
        }
    }
    return json;
}

void test_parse_scalar_values() {
    const JsonParseResult integer = parse_or_fail("  -42\n");
    expect_true(integer.value.is_int64(), "-42 should parse as int64");
    expect_equal(*integer.value.get_if_int64(), static_cast<std::int64_t>(-42), "integer value should match");

    const std::string int64_max_text = std::to_string(kInt64Max);
    const JsonParseResult int64_max = parse_or_fail(int64_max_text);
    expect_true(int64_max.value.is_int64(), "int64 max should parse as int64");
    expect_equal(*int64_max.value.get_if_int64(), kInt64Max, "int64 max value should match");

    const std::string int64_min_text = std::to_string(kInt64Min);
    const JsonParseResult int64_min = parse_or_fail(int64_min_text);
    expect_true(int64_min.value.is_int64(), "int64 min should parse as int64");
    expect_equal(*int64_min.value.get_if_int64(), kInt64Min, "int64 min value should match");

    const JsonParseResult floating = parse_or_fail("6.25e1");
    expect_true(floating.value.is_double(), "float should parse as double");
    expect_true(*floating.value.get_if_double() > 62.49 && *floating.value.get_if_double() < 62.51,
                "double value should match");

    const JsonParseResult boolean = parse_or_fail("true");
    expect_true(boolean.value.is_bool(), "true should parse as bool");
    expect_true(*boolean.value.get_if_bool(), "true bool value should match");

    const JsonParseResult null_result = parse_or_fail("null");
    expect_true(null_result.value.is_null(), "null should parse as null");
}

void test_parse_object_array_and_unicode() {
    const JsonParseResult parsed =
        parse_or_fail(R"({"name":"nebula","items":[1,true,null,"\u4f60\u597d","\uD83D\uDE00"]})");

    expect_true(parsed.value.is_object(), "root should be object");
    const JsonObject* object = parsed.value.get_if_object();
    expect_true(object != nullptr, "object should exist");

    const auto name_it = object->find("name");
    expect_true(name_it != object->end(), "name key should exist");
    expect_equal(*name_it->second.get_if_string(), std::string("nebula"), "name should match");

    const auto items_it = object->find("items");
    expect_true(items_it != object->end(), "items key should exist");
    const JsonArray* items = items_it->second.get_if_array();
    expect_true(items != nullptr, "items should be array");
    expect_equal(items->size(), static_cast<std::size_t>(5), "items size should match");

    expect_equal(*items->at(0).get_if_int64(), static_cast<std::int64_t>(1), "first item should be int64");
    expect_true(*items->at(1).get_if_bool(), "second item should be true");
    expect_true(items->at(2).is_null(), "third item should be null");
    expect_equal(*items->at(3).get_if_string(), std::string("你好"), "unicode BMP escape should decode");
    expect_equal(*items->at(4).get_if_string(), std::string("😀"), "unicode surrogate pair should decode");
}

void test_parse_errors() {
    const JsonParseResult empty = parse_json("\n\t ");
    expect_true(!empty.ok, "empty input should fail");
    expect_equal(empty.error, std::string("empty_input"), "empty input error should match");
    expect_equal(empty.error_offset, static_cast<std::size_t>(0), "empty input offset should be 0");

    const JsonParseResult trailing = parse_json("true x");
    expect_true(!trailing.ok, "trailing chars should fail");
    expect_equal(trailing.error, std::string("extra_characters"), "trailing chars error should match");
    expect_equal(trailing.error_offset, static_cast<std::size_t>(5), "trailing chars offset should match");

    const auto int64_max_u64 = static_cast<std::uint64_t>(kInt64Max);
    const std::string overflow_positive = std::to_string(int64_max_u64 + 1ULL);
    const std::string overflow_negative = std::format("-{}", int64_max_u64 + 2ULL);

    expect_parse_error("01", "invalid_number");
    expect_parse_error(overflow_positive, "invalid_number");
    expect_parse_error(overflow_negative, "invalid_number");
    expect_parse_error("\v1", "invalid_value");
    expect_parse_error("[1,\v2]", "invalid_value");
    expect_parse_error("{\f\"a\"\f:\f1\f}", "expected_string");
    expect_parse_error(R"("\q")", "invalid_escape");
    expect_parse_error(R"({"a":1,"a":2})", "duplicate_key");
    expect_parse_error(R"({"a" 1})", "expected_colon");
    expect_parse_error(R"({"a":1,})", "trailing_comma");
}

void test_parse_string_boundary_errors() {
    expect_parse_error("\"abc", "unterminated_string");
    expect_parse_error("\"abc\\", "unterminated_string");

    const JsonParseResult control_character = parse_json("{\"k\":\"line\nfeed\"}");
    expect_true(!control_character.ok, "string with raw control character should fail");
    expect_equal(control_character.error, std::string("invalid_string_character"),
                 "raw control character error should match");

    expect_parse_error(R"("\uD800\uE000")", "invalid_unicode_surrogate");
    expect_parse_error(R"("\uDC00")", "invalid_unicode_surrogate");
    expect_parse_error(R"("\u12G4")", "invalid_unicode_escape");
    expect_parse_error(R"("\u123")", "invalid_unicode_escape");

    std::string truncated_utf8 = "\"";
    truncated_utf8.push_back(static_cast<char>(0xC3));
    truncated_utf8.push_back('"');
    expect_parse_error(truncated_utf8, "invalid_utf8");

    std::string lone_continuation_utf8 = "\"";
    lone_continuation_utf8.push_back(static_cast<char>(0x80));
    lone_continuation_utf8.push_back('"');
    expect_parse_error(lone_continuation_utf8, "invalid_utf8");

    std::string invalid_three_byte_utf8 = "\"";
    invalid_three_byte_utf8.push_back(static_cast<char>(0xE2));
    invalid_three_byte_utf8.push_back(static_cast<char>(0x28));
    invalid_three_byte_utf8.push_back(static_cast<char>(0xA1));
    invalid_three_byte_utf8.push_back('"');
    expect_parse_error(invalid_three_byte_utf8, "invalid_utf8");

    std::string surrogate_utf8 = "\"";
    surrogate_utf8.push_back(static_cast<char>(0xED));
    surrogate_utf8.push_back(static_cast<char>(0xA0));
    surrogate_utf8.push_back(static_cast<char>(0x80));
    surrogate_utf8.push_back('"');
    expect_parse_error(surrogate_utf8, "invalid_utf8");
}

void test_dump_compact_sorted_keys() {
    JsonObject object;
    object.emplace("b", JsonValue(static_cast<std::int64_t>(2)));
    object.emplace("a", JsonValue("x"));
    object.emplace("c", JsonValue(nullptr));

    const std::string dumped = dump_json(JsonValue(std::move(object)));
    expect_equal(dumped, std::string(R"({"a":"x","b":2,"c":null})"), "compact dump should match");
}

void test_dump_pretty_output() {
    JsonObject object;
    object.emplace("z", JsonValue(JsonArray{JsonValue(true), JsonValue(nullptr)}));
    object.emplace("a", JsonValue(static_cast<std::int64_t>(1)));

    const std::string dumped = dump_json(JsonValue(std::move(object)), 2);
    const std::string expected =
        "{\n"
        "  \"a\": 1,\n"
        "  \"z\": [\n"
        "    true,\n"
        "    null\n"
        "  ]\n"
        "}";
    expect_equal(dumped, expected, "pretty dump should match");
}

void test_dump_invalid_utf8_string_as_unicode_escape() {
    std::string invalid_utf8;
    invalid_utf8.push_back('A');
    invalid_utf8.push_back(static_cast<char>(0xC3));
    invalid_utf8.push_back('B');
    invalid_utf8.push_back(static_cast<char>(0x80));
    invalid_utf8.push_back('C');

    const std::string dumped = dump_json(JsonValue(invalid_utf8));
    expect_equal(dumped, std::string(R"("A\u00c3B\u0080C")"), "invalid utf8 bytes should be unicode-escaped");

    const JsonParseResult reparsed = parse_or_fail(dumped);
    expect_true(reparsed.value.is_string(), "reparsed value should be string");
}

void test_round_trip() {
    const std::string input = R"({"m":[1,2,3],"n":{"k":"v"},"p":false})";
    const JsonParseResult parsed = parse_or_fail(input);

    const std::string dumped = dump_json(parsed.value, 2);
    const JsonParseResult reparsed = parse_json(dumped);

    expect_true(reparsed.ok, "round trip parse should succeed");
    expect_true(reparsed.value == parsed.value, "round trip value should match");
}

void test_round_trip_preserves_double_integer_type() {
    const JsonParseResult parsed = parse_or_fail(R"({"a":1.0})");
    expect_true(parsed.value.is_object(), "root should be object");
    const JsonObject* parsed_object = parsed.value.get_if_object();
    expect_true(parsed_object != nullptr, "parsed object should exist");
    const auto parsed_it = parsed_object->find("a");
    expect_true(parsed_it != parsed_object->end(), "a key should exist");
    expect_true(parsed_it->second.is_double(), "a should parse as double");

    const std::string dumped = dump_json(parsed.value);
    expect_equal(dumped, std::string(R"({"a":1.0})"), "dump should keep floating-point marker");

    const JsonParseResult reparsed = parse_or_fail(dumped);
    expect_true(reparsed.value == parsed.value, "round trip should preserve double type");
}

void test_dump_non_finite_double_as_null() {
    const JsonArray values = {
        JsonValue(std::numeric_limits<double>::quiet_NaN()),
        JsonValue(std::numeric_limits<double>::infinity()),
        JsonValue(-std::numeric_limits<double>::infinity()),
        JsonValue(1.5),
    };

    const std::string dumped = dump_json(JsonValue(values));
    expect_equal(dumped, std::string(R"([null,null,null,1.5])"), "non-finite doubles should dump as null");

    const JsonParseResult reparsed = parse_or_fail(dumped);
    const JsonArray* reparsed_values = reparsed.value.get_if_array();
    expect_true(reparsed_values != nullptr, "reparsed value should be array");
    expect_equal(reparsed_values->size(), static_cast<std::size_t>(4), "reparsed array size should match");
    expect_true(reparsed_values->at(0).is_null(), "nan should become null");
    expect_true(reparsed_values->at(1).is_null(), "positive infinity should become null");
    expect_true(reparsed_values->at(2).is_null(), "negative infinity should become null");
    expect_true(reparsed_values->at(3).is_double(), "finite double should stay double");
}

void test_construct_string_from_string_view() {
    std::string backing = "nebula";
    const std::string_view view = backing;
    const JsonValue value(view);

    backing[0] = 'N';
    expect_true(value.is_string(), "string_view constructor should produce string");
    expect_equal(*value.get_if_string(), std::string("nebula"), "constructed string should be copied");
}

void test_parse_nesting_depth_limit() {
    const std::string at_limit = build_nested_array_json(kJsonNestingDepthLimit);
    const JsonParseResult at_limit_result = parse_or_fail(at_limit);
    expect_true(at_limit_result.value.is_array(), "max depth input should parse");

    const std::string above_limit = build_nested_array_json(kJsonNestingDepthLimit + 1U);
    const JsonParseResult above_limit_result = parse_json(above_limit);
    expect_true(!above_limit_result.ok, "over max depth input should fail");
    expect_equal(above_limit_result.error, std::string("max_depth_exceeded"), "depth limit error should match");
    expect_equal(above_limit_result.error_offset, kJsonNestingDepthLimit, "depth limit error offset should match");
}

void test_parse_mixed_nesting_depth_limit() {
    const std::string at_limit = build_nested_object_array_json(kJsonNestingDepthLimit);
    const JsonParseResult at_limit_result = parse_or_fail(at_limit);
    expect_true(at_limit_result.value.is_object(), "max depth mixed input should parse");

    const std::string above_limit = build_nested_object_array_json(kJsonNestingDepthLimit + 1U);
    const JsonParseResult above_limit_result = parse_json(above_limit);
    expect_true(!above_limit_result.ok, "over max depth mixed input should fail");
    expect_equal(above_limit_result.error, std::string("max_depth_exceeded"), "mixed depth limit error should match");
}

int run_json_tests() {
    const std::vector<nebula::testsupport::TestCase> tests = {
        {"parse scalar values", test_parse_scalar_values},
        {"parse object array and unicode", test_parse_object_array_and_unicode},
        {"parse errors", test_parse_errors},
        {"parse string boundary errors", test_parse_string_boundary_errors},
        {"dump compact sorted keys", test_dump_compact_sorted_keys},
        {"dump pretty output", test_dump_pretty_output},
        {"dump invalid utf8 string as unicode escape", test_dump_invalid_utf8_string_as_unicode_escape},
        {"round trip", test_round_trip},
        {"round trip preserves double integer type", test_round_trip_preserves_double_integer_type},
        {"dump non-finite double as null", test_dump_non_finite_double_as_null},
        {"construct string from string_view", test_construct_string_from_string_view},
        {"parse nesting depth limit", test_parse_nesting_depth_limit},
        {"parse mixed nesting depth limit", test_parse_mixed_nesting_depth_limit},
    };
    return nebula::testsupport::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::testsupport::run_main(run_json_tests);
}
