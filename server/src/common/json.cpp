#include "nebula/common/json.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <format>
#include <utility>

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

[[nodiscard]] bool parse_valid_utf8_sequence_length(std::string_view text, std::size_t start, std::size_t& length) {
    if (start >= text.size()) {
        return false;
    }

    const auto first = static_cast<unsigned char>(text[start]);
    if (first <= 0x7FU) {
        length = 1U;
        return true;
    }

    if (first >= 0xC2U && first <= 0xDFU) {
        length = 2U;
    } else if (first >= 0xE0U && first <= 0xEFU) {
        length = 3U;
    } else if (first >= 0xF0U && first <= 0xF4U) {
        length = 4U;
    } else {
        return false;
    }

    if ((start + length) > text.size()) {
        return false;
    }

    if (length == 2U) {
        const auto second = static_cast<unsigned char>(text[start + 1U]);
        return is_utf8_continuation_byte(second);
    }

    if (length == 3U) {
        const auto second = static_cast<unsigned char>(text[start + 1U]);
        const auto third = static_cast<unsigned char>(text[start + 2U]);
        return is_valid_utf8_second_byte_for_three_bytes(first, second) && is_utf8_continuation_byte(third);
    }

    const auto second = static_cast<unsigned char>(text[start + 1U]);
    const auto third = static_cast<unsigned char>(text[start + 2U]);
    const auto fourth = static_cast<unsigned char>(text[start + 3U]);
    return is_valid_utf8_second_byte_for_four_bytes(first, second) && is_utf8_continuation_byte(third) &&
           is_utf8_continuation_byte(fourth);
}

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    [[nodiscard]] JsonParseResult parse() {
        JsonParseResult result;
        skip_whitespace();
        if (eof()) {
            fail("empty_input", 0);
            return build_failed_result();
        }

        JsonValue value;
        if (!parse_value(value)) {
            return build_failed_result();
        }

        skip_whitespace();
        if (!eof()) {
            fail("extra_characters", cursor_);
            return build_failed_result();
        }

        result.ok = true;
        result.value = std::move(value);
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

    bool parse_value(JsonValue& value) {
        skip_whitespace();
        if (eof()) {
            return fail("unexpected_end", cursor_);
        }

        const char ch = peek();
        if (ch == '"') {
            std::string parsed_string;
            if (!parse_string(parsed_string)) {
                return false;
            }
            value = JsonValue(std::move(parsed_string));
            return true;
        }

        if (ch == '{') {
            return parse_object(value);
        }

        if (ch == '[') {
            return parse_array(value);
        }

        if (ch == 't') {
            if (!parse_literal("true")) {
                return false;
            }
            value = JsonValue(true);
            return true;
        }

        if (ch == 'f') {
            if (!parse_literal("false")) {
                return false;
            }
            value = JsonValue(false);
            return true;
        }

        if (ch == 'n') {
            if (!parse_literal("null")) {
                return false;
            }
            value = JsonValue(nullptr);
            return true;
        }

        if (ch == '-' || std::isdigit(static_cast<unsigned char>(ch)) != 0) {
            return parse_number(value);
        }

        return fail("invalid_value", cursor_);
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

    bool parse_number(JsonValue& value) {
        const std::size_t start = cursor_;
        bool is_double = false;
        if (!parse_integer_part(start)) {
            return false;
        }
        if (!parse_fraction_part(start, is_double)) {
            return false;
        }
        if (!parse_exponent_part(start, is_double)) {
            return false;
        }

        const std::string_view number = text_.substr(start, cursor_ - start);
        if (!is_double) {
            std::int64_t integer_value = 0;
            const auto [ptr, ec] = std::from_chars(number.begin(), number.data() + number.size(), integer_value);
            if (ec == std::errc() && ptr == (number.data() + number.size())) {
                value = JsonValue(integer_value);
                return true;
            }
        } else {
            double double_value = 0.0;
            const auto [ptr, ec] = std::from_chars(number.data(), number.data() + number.size(), double_value);
            if (ec == std::errc() && ptr == (number.data() + number.size())) {
                value = JsonValue(double_value);
                return true;
            }
        }
        return fail("invalid_number", start);
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

    bool parse_hex_quad(std::uint16_t& value, std::size_t sequence_offset) {
        if ((cursor_ + 4U) > text_.size()) {
            return fail("invalid_unicode_escape", sequence_offset);
        }

        value = 0;
        for (std::size_t idx = 0; idx < 4U; ++idx) {
            const char ch = text_[cursor_ + idx];
            if (!is_hex(ch)) {
                return fail("invalid_unicode_escape", sequence_offset);
            }
            value = static_cast<std::uint16_t>((value << 4U) | hex_to_int(ch));
        }
        cursor_ += 4U;
        return true;
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
        std::uint16_t first = 0;
        if (!parse_hex_quad(first, sequence_offset)) {
            return false;
        }

        if (first >= 0xDC00U && first <= 0xDFFFU) {
            return fail("invalid_unicode_surrogate", sequence_offset);
        }

        char32_t codepoint = first;
        if (first >= 0xD800U && first <= 0xDBFFU) {
            if (!consume('\\') || !consume('u')) {
                return fail("invalid_unicode_surrogate", sequence_offset);
            }
            std::uint16_t low = 0;
            if (!parse_hex_quad(low, sequence_offset)) {
                return false;
            }
            if (low < 0xDC00U || low > 0xDFFFU) {
                return fail("invalid_unicode_surrogate", sequence_offset);
            }
            codepoint =
                0x10000U + ((static_cast<char32_t>(first) - 0xD800U) << 10U) + (static_cast<char32_t>(low) - 0xDC00U);
        }

        append_utf8(codepoint, out);
        return true;
    }

    bool parse_string(std::string& value) {
        if (!consume('"')) {
            return fail("expected_string", cursor_);
        }

        const std::size_t start = cursor_ - 1U;
        while (!eof()) {
            const std::size_t byte_offset = cursor_;
            const auto byte = static_cast<unsigned char>(text_[cursor_]);
            if (byte == static_cast<unsigned char>('"')) {
                ++cursor_;
                return true;
            }

            if (byte < 0x20U) {
                return fail("invalid_string_character", byte_offset);
            }

            if (byte != static_cast<unsigned char>('\\')) {
                std::size_t utf8_length = 0;
                if (!parse_valid_utf8_sequence_length(text_, byte_offset, utf8_length)) {
                    return fail("invalid_utf8", byte_offset);
                }
                value.append(text_.data() + byte_offset, utf8_length);
                cursor_ += utf8_length;
                continue;
            }

            ++cursor_;
            if (eof()) {
                return fail("unterminated_string", start);
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
                        return false;
                    }
                    break;
                }
                default:
                    return fail("invalid_escape", cursor_ - 1U);
            }
        }

        return fail("unterminated_string", start);
    }

    bool parse_array(JsonValue& value) {
        if (nesting_depth_ >= kJsonNestingDepthLimit) {
            return fail("max_depth_exceeded", cursor_);
        }
        const NestingDepthGuard depth_guard(nesting_depth_);

        if (!consume('[')) {
            return fail("invalid_value", cursor_);
        }

        JsonArray items;
        skip_whitespace();
        if (consume(']')) {
            value = JsonValue(std::move(items));
            return true;
        }

        while (true) {
            JsonValue item;
            if (!parse_value(item)) {
                return false;
            }
            items.push_back(std::move(item));

            skip_whitespace();
            if (consume(',')) {
                skip_whitespace();
                if (peek() == ']') {
                    return fail("trailing_comma", cursor_);
                }
                continue;
            }

            if (consume(']')) {
                value = JsonValue(std::move(items));
                return true;
            }

            if (eof()) {
                return fail("unexpected_end", cursor_);
            }
            return fail("expected_comma_or_array_end", cursor_);
        }
    }

    bool parse_object(JsonValue& value) {
        if (nesting_depth_ >= kJsonNestingDepthLimit) {
            return fail("max_depth_exceeded", cursor_);
        }
        const NestingDepthGuard depth_guard(nesting_depth_);

        if (!consume('{')) {
            return fail("invalid_value", cursor_);
        }

        JsonObject object;
        skip_whitespace();
        if (consume('}')) {
            value = JsonValue(std::move(object));
            return true;
        }

        while (true) {
            skip_whitespace();
            if (peek() != '"') {
                return fail("expected_string", cursor_);
            }

            std::string key;
            if (!parse_string(key)) {
                return false;
            }

            skip_whitespace();
            if (!consume(':')) {
                return fail("expected_colon", cursor_);
            }

            JsonValue field_value;
            if (!parse_value(field_value)) {
                return false;
            }

            if (object.contains(key)) {
                return fail("duplicate_key", cursor_);
            }
            object.emplace(std::move(key), std::move(field_value));

            skip_whitespace();
            if (consume(',')) {
                skip_whitespace();
                if (peek() == '}') {
                    return fail("trailing_comma", cursor_);
                }
                continue;
            }

            if (consume('}')) {
                value = JsonValue(std::move(object));
                return true;
            }

            if (eof()) {
                return fail("unexpected_end", cursor_);
            }
            return fail("expected_comma_or_object_end", cursor_);
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
    void append_byte_as_unicode_escape(unsigned char byte) {
        output_.append(std::format("\\u{:04x}", static_cast<unsigned int>(byte)));
    }

    void append_string_escaped(std::string_view text) {
        output_.push_back('"');
        std::size_t byte_offset = 0;
        while (byte_offset < text.size()) {
            const auto ch = static_cast<unsigned char>(text[byte_offset]);
            switch (ch) {
                case '"':
                    output_.append("\\\"");
                    ++byte_offset;
                    break;
                case '\\':
                    output_.append("\\\\");
                    ++byte_offset;
                    break;
                case '\b':
                    output_.append("\\b");
                    ++byte_offset;
                    break;
                case '\f':
                    output_.append("\\f");
                    ++byte_offset;
                    break;
                case '\n':
                    output_.append("\\n");
                    ++byte_offset;
                    break;
                case '\r':
                    output_.append("\\r");
                    ++byte_offset;
                    break;
                case '\t':
                    output_.append("\\t");
                    ++byte_offset;
                    break;
                default:
                    if (ch < 0x20U) {
                        append_byte_as_unicode_escape(ch);
                        ++byte_offset;
                    } else if (ch <= 0x7FU) {
                        output_.push_back(static_cast<char>(ch));
                        ++byte_offset;
                    } else {
                        std::size_t utf8_length = 0;
                        if (parse_valid_utf8_sequence_length(text, byte_offset, utf8_length)) {
                            output_.append(text.data() + byte_offset, utf8_length);
                            byte_offset += utf8_length;
                        } else {
                            append_byte_as_unicode_escape(ch);
                            ++byte_offset;
                        }
                    }
                    break;
            }
        }
        output_.push_back('"');
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

}  // namespace nebula::common
