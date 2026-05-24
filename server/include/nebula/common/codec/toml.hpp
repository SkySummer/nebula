#ifndef NEBULA_COMMON_CODEC_TOML_HPP
#define NEBULA_COMMON_CODEC_TOML_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>

namespace nebula::common {

struct TomlValue {
    using Value = std::variant<std::string, std::int64_t, double, bool>;

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

}  // namespace nebula::common

#endif  // NEBULA_COMMON_CODEC_TOML_HPP
