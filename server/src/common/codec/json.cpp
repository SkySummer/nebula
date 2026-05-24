#include "nebula/common/codec/json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <format>
#include <optional>
#include <utility>

#include "nebula/common/base/string.hpp"

namespace nebula::common {

namespace {

[[nodiscard]] constexpr bool is_json_whitespace(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

constexpr std::size_t kJsonNestingDepthLimit = 256U;

[[nodiscard]] bool is_utf8_continuation_byte(unsigned char byte) {
    return (byte & 0xC0U) == 0x80U;
}

[[nodiscard]] bool is_valid_utf8_second_byte_for_three_bytes(unsigned char first, unsigned char second) {
    if (first == 0xE0U) {
        return second >= 0xA0U && second <= 0xBFU;
    }
    if (first == 0xEDU) {
        return second >= 0x80U && second <= 0x9FU;
    }
    return is_utf8_continuation_byte(second);
}

[[nodiscard]] bool is_valid_utf8_second_byte_for_four_bytes(unsigned char first, unsigned char second) {
    if (first == 0xF0U) {
        return second >= 0x90U && second <= 0xBFU;
    }
    if (first == 0xF4U) {
        return second >= 0x80U && second <= 0x8FU;
    }
    return is_utf8_continuation_byte(second);
}

[[nodiscard]] std::optional<std::size_t> parse_valid_utf8_sequence_length(std::string_view text, std::size_t start) {
    if (start >= text.size()) {
        return std::nullopt;
    }

    const auto first = static_cast<unsigned char>(text[start]);
    if (first <= 0x7FU) {
        return 1U;
    }

    std::size_t length = 0;
    if (first >= 0xC2U && first <= 0xDFU) {
        length = 2U;
    } else if (first >= 0xE0U && first <= 0xEFU) {
        length = 3U;
    } else if (first >= 0xF0U && first <= 0xF4U) {
        length = 4U;
    } else {
        return std::nullopt;
    }

    if ((start + length) > text.size()) {
        return std::nullopt;
    }

    if (length == 2U) {
        const auto second = static_cast<unsigned char>(text[start + 1U]);
        return is_utf8_continuation_byte(second) ? std::optional<std::size_t>(length) : std::nullopt;
    }

    if (length == 3U) {
        const auto second = static_cast<unsigned char>(text[start + 1U]);
        const auto third = static_cast<unsigned char>(text[start + 2U]);
        if (!is_valid_utf8_second_byte_for_three_bytes(first, second) || !is_utf8_continuation_byte(third)) {
            return std::nullopt;
        }
        return length;
    }

    const auto second = static_cast<unsigned char>(text[start + 1U]);
    const auto third = static_cast<unsigned char>(text[start + 2U]);
    const auto fourth = static_cast<unsigned char>(text[start + 3U]);
    if (!is_valid_utf8_second_byte_for_four_bytes(first, second) || !is_utf8_continuation_byte(third) ||
        !is_utf8_continuation_byte(fourth)) {
        return std::nullopt;
    }
    return length;
}

std::string build_json_escaped_string(std::string_view text) {
    std::string output;
    output.reserve(text.size() + 2U);
    output.push_back('"');
    std::size_t byte_offset = 0;
    while (byte_offset < text.size()) {
        const auto ch = static_cast<unsigned char>(text[byte_offset]);
        switch (ch) {
            case '"':
                output.append("\\\"");
                ++byte_offset;
                break;
            case '\\':
                output.append("\\\\");
                ++byte_offset;
                break;
            case '\b':
                output.append("\\b");
                ++byte_offset;
                break;
            case '\f':
                output.append("\\f");
                ++byte_offset;
                break;
            case '\n':
                output.append("\\n");
                ++byte_offset;
                break;
            case '\r':
                output.append("\\r");
                ++byte_offset;
                break;
            case '\t':
                output.append("\\t");
                ++byte_offset;
                break;
            default:
                if (ch < 0x20U) {
                    output.append(std::format("\\u{:04x}", static_cast<unsigned int>(ch)));
                    ++byte_offset;
                } else if (ch <= 0x7FU) {
                    output.push_back(static_cast<char>(ch));
                    ++byte_offset;
                } else {
                    std::optional<std::size_t> utf8_length = parse_valid_utf8_sequence_length(text, byte_offset);
                    if (utf8_length.has_value()) {
                        output.append(text.data() + byte_offset, *utf8_length);
                        byte_offset += *utf8_length;
                    } else {
                        output.append(std::format("\\u{:04x}", static_cast<unsigned int>(ch)));
                        ++byte_offset;
                    }
                }
                break;
        }
    }
    output.push_back('"');
    return output;
}

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    [[nodiscard]] JsonParseResult parse() {
        skip_whitespace();
        if (eof()) {
            fail("empty_input", 0);
            return build_failed_result();
        }

        std::optional<JsonValue> value = parse_value();
        if (!value.has_value()) {
            return build_failed_result();
        }

        skip_whitespace();
        if (!eof()) {
            fail("extra_characters", cursor_);
            return build_failed_result();
        }

        JsonParseResult result;
        result.ok = true;
        result.value = std::move(*value);
        return result;
    }

private:
    class NestingDepthGuard {
    public:
        explicit NestingDepthGuard(std::size_t& depth) : depth_(depth) {
            ++depth_;
        }

        ~NestingDepthGuard() noexcept {
            --depth_;
        }

        NestingDepthGuard(const NestingDepthGuard&) = delete;
        NestingDepthGuard& operator=(const NestingDepthGuard&) = delete;
        NestingDepthGuard(NestingDepthGuard&&) = delete;
        NestingDepthGuard& operator=(NestingDepthGuard&&) = delete;

    private:
        std::size_t& depth_;
    };

    [[nodiscard]] JsonParseResult build_failed_result() const {
        JsonParseResult result;
        result.ok = false;
        result.error_offset = error_offset_;
        result.error = error_;
        return result;
    }

    bool fail(std::string error, std::size_t offset) {
        if (!failed_) {
            failed_ = true;
            error_ = std::move(error);
            error_offset_ = offset;
        }
        return false;
    }

    [[nodiscard]] bool eof() const {
        return cursor_ >= text_.size();
    }

    [[nodiscard]] char peek() const {
        return eof() ? '\0' : text_[cursor_];
    }

    void skip_whitespace() {
        while (!eof() && is_json_whitespace(text_[cursor_])) {
            ++cursor_;
        }
    }

    bool consume(char expected) {
        if (peek() != expected) {
            return false;
        }
        ++cursor_;
        return true;
    }

    [[nodiscard]] std::optional<JsonValue> parse_value() {
        skip_whitespace();
        if (eof()) {
            fail("unexpected_end", cursor_);
            return std::nullopt;
        }

        const char ch = peek();
        if (ch == '"') {
            std::optional<std::string> parsed_string = parse_string();
            if (!parsed_string.has_value()) {
                return std::nullopt;
            }
            return JsonValue(std::move(*parsed_string));
        }

        if (ch == '{') {
            std::optional<JsonObject> object = parse_object();
            if (!object.has_value()) {
                return std::nullopt;
            }
            return JsonValue(std::move(*object));
        }

        if (ch == '[') {
            std::optional<JsonArray> array = parse_array();
            if (!array.has_value()) {
                return std::nullopt;
            }
            return JsonValue(std::move(*array));
        }

        if (ch == 't') {
            if (!parse_literal("true")) {
                return std::nullopt;
            }
            return JsonValue(true);
        }

        if (ch == 'f') {
            if (!parse_literal("false")) {
                return std::nullopt;
            }
            return JsonValue(false);
        }

        if (ch == 'n') {
            if (!parse_literal("null")) {
                return std::nullopt;
            }
            return JsonValue(nullptr);
        }

        if (ch == '-' || std::isdigit(static_cast<unsigned char>(ch)) != 0) {
            return parse_json_number();
        }

        fail("invalid_value", cursor_);
        return std::nullopt;
    }

    bool parse_literal(std::string_view literal) {
        if (text_.substr(cursor_, literal.size()) != literal) {
            return fail("invalid_value", cursor_);
        }
        cursor_ += literal.size();
        return true;
    }

    bool parse_integer_part(std::size_t start) {
        if (consume('-') && eof()) {
            return fail("invalid_number", start);
        }

        if (peek() == '0') {
            ++cursor_;
            if (std::isdigit(static_cast<unsigned char>(peek())) != 0) {
                return fail("invalid_number", start);
            }
            return true;
        }

        if (std::isdigit(static_cast<unsigned char>(peek())) == 0) {
            return fail("invalid_number", start);
        }
        while (std::isdigit(static_cast<unsigned char>(peek())) != 0) {
            ++cursor_;
        }
        return true;
    }

    bool parse_fraction_part(std::size_t start, bool& is_double) {
        if (!consume('.')) {
            return true;
        }
        is_double = true;
        if (std::isdigit(static_cast<unsigned char>(peek())) == 0) {
            return fail("invalid_number", start);
        }
        while (std::isdigit(static_cast<unsigned char>(peek())) != 0) {
            ++cursor_;
        }
        return true;
    }

    bool parse_exponent_part(std::size_t start, bool& is_double) {
        if (peek() != 'e' && peek() != 'E') {
            return true;
        }
        is_double = true;
        ++cursor_;
        if (peek() == '+' || peek() == '-') {
            ++cursor_;
        }
        if (std::isdigit(static_cast<unsigned char>(peek())) == 0) {
            return fail("invalid_number", start);
        }
        while (std::isdigit(static_cast<unsigned char>(peek())) != 0) {
            ++cursor_;
        }
        return true;
    }

    [[nodiscard]] std::optional<JsonValue> parse_json_number() {
        const std::size_t start = cursor_;
        bool is_double = false;
        if (!parse_integer_part(start)) {
            return std::nullopt;
        }
        if (!parse_fraction_part(start, is_double)) {
            return std::nullopt;
        }
        if (!parse_exponent_part(start, is_double)) {
            return std::nullopt;
        }

        const std::string_view number = text_.substr(start, cursor_ - start);
        if (!is_double) {
            const auto integer_value = parse_number<std::int64_t>(number);
            if (integer_value.has_value()) {
                return JsonValue(*integer_value);
            }
        } else {
            const auto double_value = parse_number<double>(number);
            if (double_value.has_value()) {
                return JsonValue(*double_value);
            }
        }
        fail("invalid_number", start);
        return std::nullopt;
    }

    static bool is_hex(char ch) {
        return std::isxdigit(static_cast<unsigned char>(ch)) != 0;
    }

    static std::uint16_t hex_to_int(char ch) {
        if (ch >= '0' && ch <= '9') {
            return static_cast<std::uint16_t>(ch - '0');
        }
        if (ch >= 'a' && ch <= 'f') {
            return static_cast<std::uint16_t>(10 + ch - 'a');
        }
        if (ch >= 'A' && ch <= 'F') {
            return static_cast<std::uint16_t>(10 + ch - 'A');
        }
        return 0;
    }

    [[nodiscard]] std::optional<std::uint16_t> parse_hex_quad(std::size_t sequence_offset) {
        if ((cursor_ + 4U) > text_.size()) {
            fail("invalid_unicode_escape", sequence_offset);
            return std::nullopt;
        }

        std::uint16_t value = 0;
        for (std::size_t idx = 0; idx < 4U; ++idx) {
            const char ch = text_[cursor_ + idx];
            if (!is_hex(ch)) {
                fail("invalid_unicode_escape", sequence_offset);
                return std::nullopt;
            }
            value = static_cast<std::uint16_t>((value << 4U) | hex_to_int(ch));
        }
        cursor_ += 4U;
        return value;
    }

    static void append_utf8(char32_t codepoint, std::string& out) {
        if (codepoint <= 0x7FU) {
            out.push_back(static_cast<char>(codepoint));
            return;
        }
        if (codepoint <= 0x7FFU) {
            out.push_back(static_cast<char>(0xC0U | ((codepoint >> 6U) & 0x1FU)));
            out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
            return;
        }
        if (codepoint <= 0xFFFFU) {
            out.push_back(static_cast<char>(0xE0U | ((codepoint >> 12U) & 0x0FU)));
            out.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
            return;
        }

        out.push_back(static_cast<char>(0xF0U | ((codepoint >> 18U) & 0x07U)));
        out.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    }

    bool parse_unicode_escape(std::string& out, std::size_t sequence_offset) {
        std::optional<std::uint16_t> first = parse_hex_quad(sequence_offset);
        if (!first.has_value()) {
            return false;
        }

        if (*first >= 0xDC00U && *first <= 0xDFFFU) {
            return fail("invalid_unicode_surrogate", sequence_offset);
        }

        char32_t codepoint = *first;
        if (*first >= 0xD800U && *first <= 0xDBFFU) {
            if (!consume('\\') || !consume('u')) {
                return fail("invalid_unicode_surrogate", sequence_offset);
            }
            std::optional<std::uint16_t> low = parse_hex_quad(sequence_offset);
            if (!low.has_value()) {
                return false;
            }
            if (*low < 0xDC00U || *low > 0xDFFFU) {
                return fail("invalid_unicode_surrogate", sequence_offset);
            }
            codepoint =
                0x10000U + ((static_cast<char32_t>(*first) - 0xD800U) << 10U) + (static_cast<char32_t>(*low) - 0xDC00U);
        }

        append_utf8(codepoint, out);
        return true;
    }

    [[nodiscard]] std::optional<std::string> parse_string() {
        if (!consume('"')) {
            fail("expected_string", cursor_);
            return std::nullopt;
        }

        std::string value;
        const std::size_t start = cursor_ - 1U;
        while (!eof()) {
            const std::size_t byte_offset = cursor_;
            const auto byte = static_cast<unsigned char>(text_[cursor_]);
            if (byte == static_cast<unsigned char>('"')) {
                ++cursor_;
                return value;
            }

            if (byte < 0x20U) {
                fail("invalid_string_character", byte_offset);
                return std::nullopt;
            }

            if (byte != static_cast<unsigned char>('\\')) {
                std::optional<std::size_t> utf8_length = parse_valid_utf8_sequence_length(text_, byte_offset);
                if (!utf8_length.has_value()) {
                    fail("invalid_utf8", byte_offset);
                    return std::nullopt;
                }
                value.append(text_.data() + byte_offset, *utf8_length);
                cursor_ += *utf8_length;
                continue;
            }

            ++cursor_;
            if (eof()) {
                fail("unterminated_string", start);
                return std::nullopt;
            }

            const char escaped = text_[cursor_++];
            switch (escaped) {
                case '"':
                    value.push_back('"');
                    break;
                case '\\':
                    value.push_back('\\');
                    break;
                case '/':
                    value.push_back('/');
                    break;
                case 'b':
                    value.push_back('\b');
                    break;
                case 'f':
                    value.push_back('\f');
                    break;
                case 'n':
                    value.push_back('\n');
                    break;
                case 'r':
                    value.push_back('\r');
                    break;
                case 't':
                    value.push_back('\t');
                    break;
                case 'u': {
                    const std::size_t sequence_offset = cursor_ - 1U;
                    if (!parse_unicode_escape(value, sequence_offset)) {
                        return std::nullopt;
                    }
                    break;
                }
                default:
                    fail("invalid_escape", cursor_ - 1U);
                    return std::nullopt;
            }
        }

        fail("unterminated_string", start);
        return std::nullopt;
    }

    [[nodiscard]] std::optional<JsonArray> parse_array() {
        if (nesting_depth_ >= kJsonNestingDepthLimit) {
            fail("max_depth_exceeded", cursor_);
            return std::nullopt;
        }
        const NestingDepthGuard depth_guard(nesting_depth_);

        if (!consume('[')) {
            fail("invalid_value", cursor_);
            return std::nullopt;
        }

        JsonArray items;
        skip_whitespace();
        if (consume(']')) {
            return items;
        }

        while (true) {
            std::optional<JsonValue> item = parse_value();
            if (!item.has_value()) {
                return std::nullopt;
            }
            items.push_back(std::move(*item));

            skip_whitespace();
            if (consume(',')) {
                skip_whitespace();
                if (peek() == ']') {
                    fail("trailing_comma", cursor_);
                    return std::nullopt;
                }
                continue;
            }

            if (consume(']')) {
                return items;
            }

            if (eof()) {
                fail("unexpected_end", cursor_);
                return std::nullopt;
            }
            fail("expected_comma_or_array_end", cursor_);
            return std::nullopt;
        }
    }

    [[nodiscard]] std::optional<JsonObject> parse_object() {
        if (nesting_depth_ >= kJsonNestingDepthLimit) {
            fail("max_depth_exceeded", cursor_);
            return std::nullopt;
        }
        const NestingDepthGuard depth_guard(nesting_depth_);

        if (!consume('{')) {
            fail("invalid_value", cursor_);
            return std::nullopt;
        }

        JsonObject object;
        skip_whitespace();
        if (consume('}')) {
            return object;
        }

        while (true) {
            skip_whitespace();
            if (peek() != '"') {
                fail("expected_string", cursor_);
                return std::nullopt;
            }

            std::optional<std::string> key = parse_string();
            if (!key.has_value()) {
                return std::nullopt;
            }

            skip_whitespace();
            if (!consume(':')) {
                fail("expected_colon", cursor_);
                return std::nullopt;
            }

            std::optional<JsonValue> field_value = parse_value();
            if (!field_value.has_value()) {
                return std::nullopt;
            }

            if (object.contains(*key)) {
                fail("duplicate_key", cursor_);
                return std::nullopt;
            }
            object.emplace(std::move(*key), std::move(*field_value));

            skip_whitespace();
            if (consume(',')) {
                skip_whitespace();
                if (peek() == '}') {
                    fail("trailing_comma", cursor_);
                    return std::nullopt;
                }
                continue;
            }

            if (consume('}')) {
                return object;
            }

            if (eof()) {
                fail("unexpected_end", cursor_);
                return std::nullopt;
            }
            fail("expected_comma_or_object_end", cursor_);
            return std::nullopt;
        }
    }

    std::string_view text_;
    std::size_t cursor_ = 0;
    std::size_t nesting_depth_ = 0;
    bool failed_ = false;
    std::size_t error_offset_ = 0;
    std::string error_;
};

class JsonDumper {
public:
    explicit JsonDumper(std::size_t indent) : indent_(indent) {}

    [[nodiscard]] std::string dump(const JsonValue& value) {
        dump_value(value, 0);
        return output_;
    }

private:
    void append_string_escaped(std::string_view text) {
        output_.append(build_json_escaped_string(text));
    }

    void append_spaces(std::size_t count) {
        output_.append(count, ' ');
    }

    void append_number(std::int64_t value) {
        output_.append(std::format("{}", value));
    }

    void append_number(double value) {
        if (!std::isfinite(value)) {
            output_.append("null");
            return;
        }
        std::string dumped = std::format("{:.17g}", value);
        if (dumped.find_first_of(".eE") == std::string::npos) {
            dumped.append(".0");
        }
        output_.append(dumped);
    }

    void dump_array(const JsonArray& array, std::size_t level) {
        output_.push_back('[');
        if (array.empty()) {
            output_.push_back(']');
            return;
        }

        if (indent_ == 0U) {
            for (std::size_t idx = 0; idx < array.size(); ++idx) {
                dump_value(array[idx], level);
                if (idx + 1U < array.size()) {
                    output_.push_back(',');
                }
            }
            output_.push_back(']');
            return;
        }

        output_.push_back('\n');
        for (std::size_t idx = 0; idx < array.size(); ++idx) {
            append_spaces(level + indent_);
            dump_value(array[idx], level + indent_);
            if (idx + 1U < array.size()) {
                output_.push_back(',');
            }
            output_.push_back('\n');
        }
        append_spaces(level);
        output_.push_back(']');
    }

    void dump_object(const JsonObject& object, std::size_t level) {
        output_.push_back('{');
        if (object.empty()) {
            output_.push_back('}');
            return;
        }

        std::vector<const std::pair<const std::string, JsonValue>*> entries;
        entries.reserve(object.size());
        for (const auto& entry : object) {
            entries.push_back(&entry);
        }
        std::ranges::sort(entries, [](const auto* lhs, const auto* rhs) { return lhs->first < rhs->first; });

        if (indent_ == 0U) {
            for (std::size_t idx = 0; idx < entries.size(); ++idx) {
                append_string_escaped(entries[idx]->first);
                output_.push_back(':');
                dump_value(entries[idx]->second, level);
                if (idx + 1U < entries.size()) {
                    output_.push_back(',');
                }
            }
            output_.push_back('}');
            return;
        }

        output_.push_back('\n');
        for (std::size_t idx = 0; idx < entries.size(); ++idx) {
            append_spaces(level + indent_);
            append_string_escaped(entries[idx]->first);
            output_.append(": ");
            dump_value(entries[idx]->second, level + indent_);
            if (idx + 1U < entries.size()) {
                output_.push_back(',');
            }
            output_.push_back('\n');
        }
        append_spaces(level);
        output_.push_back('}');
    }

    void dump_value(const JsonValue& value, std::size_t level) {
        std::visit(
            [this, level](const auto& typed_value) {
                using Type = std::decay_t<decltype(typed_value)>;
                if constexpr (std::is_same_v<Type, std::nullptr_t>) {
                    output_.append("null");
                } else if constexpr (std::is_same_v<Type, bool>) {
                    output_.append(typed_value ? "true" : "false");
                } else if constexpr (std::is_same_v<Type, std::int64_t> || std::is_same_v<Type, double>) {
                    append_number(typed_value);
                } else if constexpr (std::is_same_v<Type, std::string>) {
                    append_string_escaped(typed_value);
                } else if constexpr (std::is_same_v<Type, JsonArray>) {
                    dump_array(typed_value, level);
                } else if constexpr (std::is_same_v<Type, JsonObject>) {
                    dump_object(typed_value, level);
                }
            },
            value.value);
    }

    std::size_t indent_ = 0;
    std::string output_;
};

}  // namespace

JsonValue::JsonValue(std::nullptr_t null_value) : value(null_value) {}
JsonValue::JsonValue(bool bool_value) : value(bool_value) {}
JsonValue::JsonValue(std::int64_t integer_value) : value(integer_value) {}
JsonValue::JsonValue(double double_value) : value(double_value) {}
JsonValue::JsonValue(std::string string_value) : value(std::move(string_value)) {}
JsonValue::JsonValue(std::string_view string_value) : value(std::string(string_value)) {}
JsonValue::JsonValue(const char* string_value) : value(std::string(string_value != nullptr ? string_value : "")) {}
JsonValue::JsonValue(JsonArray array_value) : value(std::move(array_value)) {}
JsonValue::JsonValue(JsonObject object_value) : value(std::move(object_value)) {}

bool JsonValue::is_null() const {
    return std::holds_alternative<std::nullptr_t>(value);
}

bool JsonValue::is_bool() const {
    return std::holds_alternative<bool>(value);
}

bool JsonValue::is_int64() const {
    return std::holds_alternative<std::int64_t>(value);
}

bool JsonValue::is_double() const {
    return std::holds_alternative<double>(value);
}

bool JsonValue::is_string() const {
    return std::holds_alternative<std::string>(value);
}

bool JsonValue::is_array() const {
    return std::holds_alternative<JsonArray>(value);
}

bool JsonValue::is_object() const {
    return std::holds_alternative<JsonObject>(value);
}

const bool* JsonValue::get_if_bool() const {
    return std::get_if<bool>(&value);
}

bool* JsonValue::get_if_bool() {
    return std::get_if<bool>(&value);
}

const std::int64_t* JsonValue::get_if_int64() const {
    return std::get_if<std::int64_t>(&value);
}

std::int64_t* JsonValue::get_if_int64() {
    return std::get_if<std::int64_t>(&value);
}

const double* JsonValue::get_if_double() const {
    return std::get_if<double>(&value);
}

double* JsonValue::get_if_double() {
    return std::get_if<double>(&value);
}

const std::string* JsonValue::get_if_string() const {
    return std::get_if<std::string>(&value);
}

std::string* JsonValue::get_if_string() {
    return std::get_if<std::string>(&value);
}

const JsonArray* JsonValue::get_if_array() const {
    return std::get_if<JsonArray>(&value);
}

JsonArray* JsonValue::get_if_array() {
    return std::get_if<JsonArray>(&value);
}

const JsonObject* JsonValue::get_if_object() const {
    return std::get_if<JsonObject>(&value);
}

JsonObject* JsonValue::get_if_object() {
    return std::get_if<JsonObject>(&value);
}

JsonParseResult parse_json(std::string_view text) {
    JsonParser parser(text);
    return parser.parse();
}

std::string dump_json(const JsonValue& value, std::size_t indent) {
    JsonDumper dumper(indent);
    return dumper.dump(value);
}

std::string escape_json_string(std::string_view text) {
    return build_json_escaped_string(text);
}

}  // namespace nebula::common
