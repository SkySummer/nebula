#include "nebula/database/connection_info.hpp"

#include <format>
#include <string_view>

namespace nebula::database {

namespace {

std::string quote_connection_info_value(std::string_view value) {
    std::string quoted;
    quoted.reserve(value.size() + 2U);
    quoted.push_back('\'');
    for (const char ch : value) {
        if (ch == '\\' || ch == '\'') {
            quoted.push_back('\\');
        }
        quoted.push_back(ch);
    }
    quoted.push_back('\'');
    return quoted;
}

}  // namespace

std::string build_connection_info(const DatabaseConfig& config) {
    return std::format("host={} port={} dbname={} user={} password={} connect_timeout={}",
                       quote_connection_info_value(config.host), config.port, quote_connection_info_value(config.name),
                       quote_connection_info_value(config.user), quote_connection_info_value(config.password),
                       config.connect_timeout_s);
}

}  // namespace nebula::database
