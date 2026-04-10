#ifndef NEBULA_COMMON_JSON_HPP
#define NEBULA_COMMON_JSON_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace nebula::common {

struct JsonValue;

using JsonArray = std::vector<JsonValue>;
using JsonObject = std::unordered_map<std::string, JsonValue>;

struct JsonValue {
    using Value = std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, JsonArray, JsonObject>;

    Value value = nullptr;

    JsonValue() = default;
    explicit JsonValue(std::nullptr_t null_value);
    explicit JsonValue(bool bool_value);
    explicit JsonValue(std::int64_t integer_value);
    explicit JsonValue(double double_value);
    explicit JsonValue(std::string string_value);
    explicit JsonValue(std::string_view string_value);
    explicit JsonValue(const char* string_value);
    explicit JsonValue(JsonArray array_value);
    explicit JsonValue(JsonObject object_value);

    [[nodiscard]] bool is_null() const;
    [[nodiscard]] bool is_bool() const;
    [[nodiscard]] bool is_int64() const;
    [[nodiscard]] bool is_double() const;
    [[nodiscard]] bool is_string() const;
    [[nodiscard]] bool is_array() const;
    [[nodiscard]] bool is_object() const;

    template <typename Type>
    [[nodiscard]] bool is() const {
        return std::holds_alternative<Type>(value);
    }

    [[nodiscard]] const bool* get_if_bool() const;
    [[nodiscard]] bool* get_if_bool();

    [[nodiscard]] const std::int64_t* get_if_int64() const;
    [[nodiscard]] std::int64_t* get_if_int64();

    [[nodiscard]] const double* get_if_double() const;
    [[nodiscard]] double* get_if_double();

    [[nodiscard]] const std::string* get_if_string() const;
    [[nodiscard]] std::string* get_if_string();

    [[nodiscard]] const JsonArray* get_if_array() const;
    [[nodiscard]] JsonArray* get_if_array();

    [[nodiscard]] const JsonObject* get_if_object() const;
    [[nodiscard]] JsonObject* get_if_object();

    template <typename Type>
    [[nodiscard]] const Type* get_if() const {
        return std::get_if<Type>(&value);
    }

    template <typename Type>
    [[nodiscard]] Type* get_if() {
        return std::get_if<Type>(&value);
    }

    bool operator==(const JsonValue& rhs) const = default;
};

struct JsonParseResult {
    bool ok = false;
    JsonValue value;
    std::size_t error_offset = 0;
    std::string error;
};

[[nodiscard]] JsonParseResult parse_json(std::string_view text);
[[nodiscard]] std::string dump_json(const JsonValue& value, std::size_t indent = 0);

}  // namespace nebula::common

#endif  // NEBULA_COMMON_JSON_HPP
