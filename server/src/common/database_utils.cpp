#include "nebula/common/database_utils.hpp"

#include <cstdlib>
#include <pqxx/pqxx>
#include <utility>

namespace nebula::common {

std::optional<std::string> resolve_database_password(const std::string& password_env) {
    if (password_env.empty()) {
        return std::nullopt;
    }

    const char* env_value = std::getenv(password_env.c_str());
    if (env_value == nullptr || *env_value == '\0') {
        return std::nullopt;
    }
    return std::string(env_value);
}

PostgresRowCheckStatus check_postgres_row_non_null_fields(const pqxx::row& row, std::size_t expected_size) {
    if (std::cmp_not_equal(row.size(), expected_size)) {
        return PostgresRowCheckStatus::InvalidSize;
    }
    for (const pqxx::field& field : row) {
        if (field.is_null()) {
            return PostgresRowCheckStatus::NullField;
        }
    }
    return PostgresRowCheckStatus::Ready;
}

}  // namespace nebula::common
