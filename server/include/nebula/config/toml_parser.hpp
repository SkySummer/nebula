#ifndef NEBULA_CONFIG_TOML_PARSER_HPP
#define NEBULA_CONFIG_TOML_PARSER_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>

namespace nebula::config {

struct TomlValue {
    using Value = std::variant<std::string, std::int64_t, bool>;

    Value value;
    std::size_t line = 0;
};

struct TomlParseResult {
    bool ok = false;
    std::unordered_map<std::string, TomlValue> table;
    std::size_t error_line = 0;
    std::string error;
};

TomlParseResult parse_toml(std::string_view text);

}  // namespace nebula::config

#endif  // NEBULA_CONFIG_TOML_PARSER_HPP
