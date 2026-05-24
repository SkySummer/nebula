#include "nebula/auth/repository/repository.hpp"

#include <cstdint>
#include <exception>
#include <optional>
#include <pqxx/pqxx>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "nebula/auth/repository/executor.hpp"
#include "nebula/auth/repository/row_parser.hpp"
#include "nebula/common/log/logger.hpp"
#include "nebula/database/connection_pool.hpp"
#include "nebula/database/row_check.hpp"

namespace nebula::auth {

namespace {

void log_query_error(std::string_view operation, const char* error) {
    common::Logger::instance()
        .error("user postgres query failed")
        .field("operation", operation)
        .field("error", error != nullptr ? error : "unknown")
        .field("decision", "return_internal_error");
}

void log_sql_error(std::string_view operation, const pqxx::sql_error& error) {
    common::Logger::instance()
        .error("user postgres query failed")
        .field("operation", operation)
        .field("sql_state", error.sqlstate())
        .field("error", error.what())
        .field("decision", "return_internal_error");
}

}  // namespace

AuthRepository::AuthRepository(std::shared_ptr<database::ConnectionPool> database_pool)
    : database_pool_(std::move(database_pool)) {
    if (database_pool_ == nullptr) {
        throw std::invalid_argument("database_pool_missing");
    }
}

bool AuthRepository::check_schema_ready() {
    const auto lease = database_pool_->acquire_lease();
    if (!lease.has_value()) {
        return false;
    }

    try {
        pqxx::read_transaction tx(lease->connection());
        const pqxx::row row = execute_check_auth_repository_schema_ready(tx);

        const database::RowCheckStatus row_status = database::check_row_ready(row, 3);
        switch (row_status) {
            case database::RowCheckStatus::InvalidSize:
                common::Logger::instance()
                    .error("auth repository readiness check failed")
                    .field("error", "repository_check_result_invalid")
                    .field("decision", "return_not_ready");
                return false;
            case database::RowCheckStatus::NullField:
                common::Logger::instance()
                    .error("auth repository readiness check failed")
                    .field("error", "repository_object_missing")
                    .field("decision", "return_not_ready");
                return false;
            case database::RowCheckStatus::Ready:
                common::Logger::instance().info("auth repository is ready");
                return true;
        }
        std::unreachable();
    } catch (const std::exception& e) {
        common::Logger::instance()
            .error("auth repository readiness check failed")
            .field("error", e.what())
            .field("decision", "return_not_ready");
        return false;
    }
}

std::expected<UserAuthRecord, UserCreateError> AuthRepository::create_user(std::string_view username,
                                                                           const PasswordHashValue& password_hash,
                                                                           std::int64_t now_s) {
    const auto lease = database_pool_->acquire_lease();
    if (!lease.has_value()) {
        return std::unexpected(UserCreateError::InternalError);
    }
    if (username.empty() || password_hash.algorithm.empty() || password_hash.salt.empty() ||
        password_hash.derived_key.empty() || now_s <= 0) {
        return std::unexpected(UserCreateError::InternalError);
    }

    try {
        pqxx::work tx(lease->connection());
        execute_acquire_global_users_mutation_advisory_lock(tx);
        const pqxx::result rows = execute_create_user(tx, username, password_hash, now_s);
        tx.commit();

        if (rows.empty()) {
            return std::unexpected(UserCreateError::DuplicateUsername);
        }

        const std::optional<UserAuthRecord> info = parse_user_auth_row(rows.front());
        if (!info.has_value()) {
            return std::unexpected(UserCreateError::InternalError);
        }
        return *info;
    } catch (const pqxx::sql_error& e) {
        log_sql_error("create_user", e);
        return std::unexpected(UserCreateError::InternalError);
    } catch (const std::exception& e) {
        log_query_error("create_user", e.what());
        return std::unexpected(UserCreateError::InternalError);
    }
}

std::expected<UserAuthRecord, UserFindError> AuthRepository::find_user_by_username(std::string_view username) {
    const auto lease = database_pool_->acquire_lease();
    if (!lease.has_value()) {
        return std::unexpected(UserFindError::InternalError);
    }

    try {
        pqxx::read_transaction tx(lease->connection());
        const pqxx::result rows = execute_find_user_by_username(tx, username);
        if (rows.empty()) {
            return std::unexpected(UserFindError::NotFound);
        }

        const std::optional<UserAuthRecord> info = parse_user_auth_row(rows.front());
        if (!info.has_value()) {
            return std::unexpected(UserFindError::InternalError);
        }
        return *info;
    } catch (const std::exception& e) {
        log_query_error("find_by_username", e.what());
        return std::unexpected(UserFindError::InternalError);
    }
}

std::expected<UserAuthRecord, UserFindError> AuthRepository::find_user_by_id(std::int64_t user_id) {
    if (user_id <= 0) {
        return std::unexpected(UserFindError::NotFound);
    }

    const auto lease = database_pool_->acquire_lease();
    if (!lease.has_value()) {
        return std::unexpected(UserFindError::InternalError);
    }

    try {
        pqxx::read_transaction tx(lease->connection());
        const pqxx::result rows = execute_find_user_by_id(tx, user_id);
        if (rows.empty()) {
            return std::unexpected(UserFindError::NotFound);
        }

        const std::optional<UserAuthRecord> info = parse_user_auth_row(rows.front());
        if (!info.has_value()) {
            return std::unexpected(UserFindError::InternalError);
        }
        return *info;
    } catch (const std::exception& e) {
        log_query_error("find_by_user_id", e.what());
        return std::unexpected(UserFindError::InternalError);
    }
}

std::expected<UserAuthRecord, UserPasswordUpdateError> AuthRepository::update_password_hash(
    std::int64_t user_id, const PasswordHashValue& password_hash, std::int64_t token_version) {
    if (user_id <= 0 || password_hash.algorithm.empty() || password_hash.salt.empty() ||
        password_hash.derived_key.empty() || token_version <= 0) {
        return std::unexpected(UserPasswordUpdateError::InternalError);
    }

    const auto lease = database_pool_->acquire_lease();
    if (!lease.has_value()) {
        return std::unexpected(UserPasswordUpdateError::InternalError);
    }

    try {
        pqxx::work tx(lease->connection());
        const pqxx::result rows = execute_update_user_password_hash(tx, user_id, password_hash, token_version);
        tx.commit();

        if (rows.empty()) {
            return std::unexpected(UserPasswordUpdateError::NotFound);
        }

        const std::optional<UserAuthRecord> info = parse_user_auth_row(rows.front());
        if (!info.has_value()) {
            return std::unexpected(UserPasswordUpdateError::InternalError);
        }
        return *info;
    } catch (const pqxx::sql_error& e) {
        log_sql_error("update_password_hash", e);
        return std::unexpected(UserPasswordUpdateError::InternalError);
    } catch (const std::exception& e) {
        log_query_error("update_password_hash", e.what());
        return std::unexpected(UserPasswordUpdateError::InternalError);
    }
}

std::optional<UserListPage> AuthRepository::list_users(std::int64_t limit, std::int64_t offset) {
    if (limit <= 0 || offset < 0) {
        return std::nullopt;
    }

    const auto lease = database_pool_->acquire_lease();
    if (!lease.has_value()) {
        return std::nullopt;
    }

    try {
        pqxx::read_transaction tx(lease->connection());
        const pqxx::result rows = execute_list_users(tx, limit, offset);

        std::vector<UserProfile> users;
        users.reserve(rows.size());
        for (const pqxx::row& row : rows) {
            const std::optional<UserProfile> info = parse_user_profile_row(row);
            if (!info.has_value()) {
                return std::nullopt;
            }
            users.push_back(*info);
        }

        bool has_more = false;
        if (std::cmp_greater(users.size(), limit)) {
            has_more = true;
            users.resize(static_cast<std::size_t>(limit));
        }

        return UserListPage{
            .users = std::move(users),
            .limit = limit,
            .offset = offset,
            .has_more = has_more,
        };
    } catch (const std::exception& e) {
        log_query_error("list_users", e.what());
        return std::nullopt;
    }
}

std::expected<UserProfile, UserRoleStatusUpdateError> AuthRepository::update_user(std::int64_t user_id,
                                                                                  std::optional<UserRole> role,
                                                                                  std::optional<UserStatus> status) {
    if (user_id <= 0) {
        return std::unexpected(UserRoleStatusUpdateError::NotFound);
    }

    const auto lease = database_pool_->acquire_lease();
    if (!lease.has_value()) {
        return std::unexpected(UserRoleStatusUpdateError::InternalError);
    }

    try {
        pqxx::work tx(lease->connection());
        execute_acquire_global_users_mutation_advisory_lock(tx);

        const pqxx::result current_rows = execute_find_user_profile_for_update(tx, user_id);
        if (current_rows.empty()) {
            return std::unexpected(UserRoleStatusUpdateError::NotFound);
        }

        const std::optional<UserProfile> current = parse_user_profile_row(current_rows.front());
        if (!current.has_value()) {
            return std::unexpected(UserRoleStatusUpdateError::InternalError);
        }

        const UserRole next_role = role.value_or(current->role);
        const UserStatus next_status = status.value_or(current->status);
        if (next_role == current->role && next_status == current->status) {
            tx.commit();
            return *current;
        }

        if (current->role == UserRole::Owner && current->status == UserStatus::Active &&
            (next_role != UserRole::Owner || next_status != UserStatus::Active)) {
            const auto active_owner_count = execute_count_active_owners(tx);
            if (active_owner_count <= 1) {
                return std::unexpected(UserRoleStatusUpdateError::LastOwnerRequired);
            }
        }

        const pqxx::result updated_rows = execute_update_user(tx, user_id, next_role, next_status);
        tx.commit();

        if (updated_rows.empty()) {
            return std::unexpected(UserRoleStatusUpdateError::NotFound);
        }

        const std::optional<UserProfile> info = parse_user_profile_row(updated_rows.front());
        if (!info.has_value()) {
            return std::unexpected(UserRoleStatusUpdateError::InternalError);
        }
        return *info;
    } catch (const pqxx::sql_error& e) {
        log_sql_error("update_user", e);
        return std::unexpected(UserRoleStatusUpdateError::InternalError);
    } catch (const std::exception& e) {
        log_query_error("update_user", e.what());
        return std::unexpected(UserRoleStatusUpdateError::InternalError);
    }
}

}  // namespace nebula::auth
