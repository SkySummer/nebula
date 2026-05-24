#include "nebula/auth/repository/row_parser.hpp"

#include <utility>

#include "nebula/database/row_check.hpp"

namespace nebula::auth {

std::optional<UserAuthRecord> parse_user_auth_row(const pqxx::row& row) {
    if (database::check_row_ready(row, 10) != database::RowCheckStatus::Ready) {
        return std::nullopt;
    }

    const auto user_id = row[0].as<std::int64_t>(0);
    if (user_id <= 0) {
        return std::nullopt;
    }

    const auto iterations = row[3].as<std::int64_t>(0);
    if (iterations <= 0 || !std::in_range<std::uint32_t>(iterations)) {
        return std::nullopt;
    }

    const auto username = row[1].as<std::string>();
    const auto algorithm = row[2].as<std::string>();
    const auto salt = row[4].as<std::string>();
    const auto derived_key = row[5].as<std::string>();
    const auto role_text = row[6].as<std::string>();
    const auto status_text = row[7].as<std::string>();
    const std::optional<UserRole> role = parse_user_role(role_text);
    const std::optional<UserStatus> status = parse_user_status(status_text);
    const auto created_at_s = row[8].as<std::int64_t>(0);
    const auto token_version = row[9].as<std::int64_t>(0);
    if (username.empty() || algorithm.empty() || salt.empty() || derived_key.empty() || !role.has_value() ||
        !status.has_value() || created_at_s <= 0 || token_version <= 0) {
        return std::nullopt;
    }

    return UserAuthRecord{
        .profile{
            .user_id = user_id,
            .username = username,
            .role = *role,
            .status = *status,
            .created_at_s = created_at_s,
        },
        .password_hash{
            .algorithm = algorithm,
            .iterations = static_cast<std::uint32_t>(iterations),
            .salt = salt,
            .derived_key = derived_key,
        },
        .token_version = token_version,
    };
}

std::optional<UserProfile> parse_user_profile_row(const pqxx::row& row) {
    if (database::check_row_ready(row, 5) != database::RowCheckStatus::Ready) {
        return std::nullopt;
    }

    const auto user_id = row[0].as<std::int64_t>(0);
    if (user_id <= 0) {
        return std::nullopt;
    }

    const auto username = row[1].as<std::string>();
    const auto role_text = row[2].as<std::string>();
    const auto status_text = row[3].as<std::string>();
    const std::optional<UserRole> role = parse_user_role(role_text);
    const std::optional<UserStatus> status = parse_user_status(status_text);
    const auto created_at_s = row[4].as<std::int64_t>(0);
    if (username.empty() || !role.has_value() || !status.has_value() || created_at_s <= 0) {
        return std::nullopt;
    }

    return UserProfile{
        .user_id = user_id,
        .username = username,
        .role = *role,
        .status = *status,
        .created_at_s = created_at_s,
    };
}

}  // namespace nebula::auth
