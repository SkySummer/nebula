#include "nebula/common/codec/toml.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <utility>

#include "nebula/common/base/string.hpp"

namespace nebula::common {

namespace {

TomlParseResult build_failed_result(std::size_t line, std::string error) {
    TomlParseResult result;
    result.ok = false;
    result.error_line = line;
    result.error = std::move(error);
    return result;
}

bool is_valid_token_char(unsigned char ch) {
    return std::isalnum(ch) != 0 || ch == '_' || ch == '-';
}

bool is_valid_token(std::string_view text) {
    if (text.empty()) {
        return false;
    }
    return std::ranges::all_of(text, is_valid_token_char);
}

std::string_view strip_comment(std::string_view line) {
    bool in_string = false;
    bool escaped = false;

    for (std::size_t idx = 0; idx < line.size(); ++idx) {
        const char ch = line[idx];
        if (in_string) {
            if (escaped) {
                escaped = false;
                continue;
            }
            if (ch == '\\') {
                escaped = true;
                continue;
            }
            if (ch == '"') {
                in_string = false;
            }
            continue;
        }

        if (ch == '"') {
            in_string = true;
            continue;
        }
        if (ch == '#') {
            return line.substr(0, idx);
        }
    }

    return line;
}

bool parse_string_value(std::string_view text, std::string& value, std::string& error) {
    if (text.size() < 2U || text.front() != '"' || text.back() != '"') {
        error = "unterminated_string";
        return false;
    }

    value.reserve(text.size() - 2U);

    for (std::size_t idx = 1U; idx + 1U < text.size(); ++idx) {
        const char ch = text[idx];
        if (ch == '"') {
            error = "invalid_string_quote";
            return false;
        }

        if (ch != '\\') {
            value.push_back(ch);
            continue;
        }

        if (idx + 2U >= text.size()) {
            error = "unterminated_string_escape";
            return false;
        }

        const char escaped = text[++idx];
        switch (escaped) {
            case '"':
                value.push_back('"');
                break;
            case '\\':
                value.push_back('\\');
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
            default:
                error = "invalid_string_escape";
                return false;
        }
    }

    return true;
}

bool is_float_literal(std::string_view text) {
    return text.find_first_of(".eE") != std::string_view::npos;
}

std::variant<std::int64_t, double, std::string> parse_numeric_value(std::string_view text) {
    if (is_float_literal(text)) {
        const auto value = parse_number<double>(text);
        if (!value.has_value()) {
            return value.error() == ParseNumberError::OutOfRange ? "float_out_of_range" : "invalid_float";
        }
        return *value;
    }

    const auto value = parse_number<std::int64_t>(text);
    if (!value.has_value()) {
        return value.error() == ParseNumberError::OutOfRange ? "integer_out_of_range" : "invalid_integer";
    }
    return *value;
}

std::string build_full_key(std::string_view section, std::string_view key) {
    if (section.empty()) {
        return std::string(key);
    }
    return std::format("{}.{}", section, key);
}

bool parse_section_line(std::string_view trimmed, std::string& section, std::string& error) {
    if (trimmed.size() < 3U || trimmed.back() != ']') {
        error = "invalid_section";
        return false;
    }

    const std::string_view name = common::trim_ascii(trimmed.substr(1U, trimmed.size() - 2U));
    if (!is_valid_token(name)) {
        error = "invalid_section_name";
        return false;
    }

    section = name;
    return true;
}

bool parse_scalar_value(std::string_view raw_value, std::size_t line_number, TomlValue& parsed_value,
                        std::string& error) {
    parsed_value.line = line_number;

    if (raw_value.front() == '"') {
        std::string parsed_string;
        if (!parse_string_value(raw_value, parsed_string, error)) {
            return false;
        }
        parsed_value.value = std::move(parsed_string);
        return true;
    }

    if (raw_value == "true") {
        parsed_value.value = true;
        return true;
    }
    if (raw_value == "false") {
        parsed_value.value = false;
        return true;
    }
    if (raw_value.front() != '-' && std::isdigit(static_cast<unsigned char>(raw_value.front())) == 0) {
        error = "unsupported_value_type";
        return false;
    }

    const auto parsed = parse_numeric_value(raw_value);
    if (const auto* error_text = std::get_if<std::string>(&parsed); error_text != nullptr) {
        error = *error_text;
        return false;
    }

    if (const auto* integer_value = std::get_if<std::int64_t>(&parsed); integer_value != nullptr) {
        parsed_value.value = *integer_value;
        return true;
    }

    if (const auto* double_value = std::get_if<double>(&parsed); double_value != nullptr) {
        parsed_value.value = *double_value;
        return true;
    }

    error = "invalid_numeric_value";
    return false;
}

bool parse_key_value_line(std::string_view trimmed, std::string_view section, std::size_t line_number,
                          TomlParseResult& result, std::string& error) {
    const std::size_t eq = trimmed.find('=');
    if (eq == std::string_view::npos) {
        error = "missing_equal";
        return false;
    }

    const std::string_view raw_key = common::trim_ascii(trimmed.substr(0U, eq));
    const std::string_view raw_value = common::trim_ascii(trimmed.substr(eq + 1U));
    if (!is_valid_token(raw_key)) {
        error = "invalid_key";
        return false;
    }
    if (raw_value.empty()) {
        error = "missing_value";
        return false;
    }

    TomlValue parsed_value;
    if (!parse_scalar_value(raw_value, line_number, parsed_value, error)) {
        return false;
    }

    std::string full_key = build_full_key(section, raw_key);
    if (result.table.contains(full_key)) {
        error = "duplicate_key";
        return false;
    }

    result.table.emplace(std::move(full_key), std::move(parsed_value));
    return true;
}

bool parse_non_empty_line(std::string_view trimmed, std::size_t line_number, std::string& section,
                          TomlParseResult& result, std::string& error) {
    if (trimmed.front() == '[') {
        return parse_section_line(trimmed, section, error);
    }
    return parse_key_value_line(trimmed, section, line_number, result, error);
}

}  // namespace

TomlParseResult parse_toml(std::string_view text) {
    TomlParseResult result;
    result.ok = true;

    std::string section;
    std::size_t line_number = 1;
    std::size_t cursor = 0;

    while (cursor <= text.size()) {
        const std::size_t line_end = text.find('\n', cursor);
        const bool has_newline = line_end != std::string_view::npos;
        std::string_view line = has_newline ? text.substr(cursor, line_end - cursor) : text.substr(cursor);

        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1U);
        }

        const std::string_view without_comment = strip_comment(line);
        const std::string_view trimmed = common::trim_ascii(without_comment);

        if (!trimmed.empty()) {
            std::string parse_error;
            if (!parse_non_empty_line(trimmed, line_number, section, result, parse_error)) {
                return build_failed_result(line_number, parse_error);
            }
        }

        if (!has_newline) {
            break;
        }

        cursor = line_end + 1U;
        ++line_number;
    }

    return result;
}

}  // namespace nebula::common
