#ifndef NEBULA_COMMON_DATABASE_UTILS_HPP
#define NEBULA_COMMON_DATABASE_UTILS_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <pqxx/pqxx>
#include <string>

namespace nebula::common {

enum class PostgresRowCheckStatus : std::uint8_t {
    Ready,
    InvalidSize,
    NullField,
};

[[nodiscard]] std::optional<std::string> resolve_database_password(const std::string& password_env);

[[nodiscard]] PostgresRowCheckStatus check_postgres_row_non_null_fields(const pqxx::row& row,
                                                                        std::size_t expected_size);

}  // namespace nebula::common

#endif  // NEBULA_COMMON_DATABASE_UTILS_HPP
