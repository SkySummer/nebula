#include "nebula/auth/user_repository.hpp"

#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <pqxx/pqxx>
#include <string>
#include <string_view>
#include <utility>

#include "nebula/common/database_utils.hpp"
#include "nebula/common/logger.hpp"
#include "nebula/common/postgres_connection_pool.hpp"

namespace nebula::auth {

namespace {

std::optional<UserInfo> parse_user_row(const pqxx::row& row) {
    if (row.size() < 7 || row[0].is_null() || row[1].is_null() || row[2].is_null() || row[3].is_null() ||
        row[4].is_null() || row[5].is_null() || row[6].is_null()) {
        return std::nullopt;
    }

    const auto user_id = row[0].as<std::int64_t>(0);
    if (user_id <= 0) {
        return std::nullopt;
    }
    const auto iterations = row[3].as<std::int64_t>(0);
    if (iterations <= 0 || std::cmp_greater(iterations, std::numeric_limits<std::uint32_t>::max())) {
        return std::nullopt;
    }

    UserInfo info;
    info.user_id = user_id;
    info.username = row[1].as<std::string>();
    info.password_hash.algorithm = row[2].as<std::string>();
    info.password_hash.iterations = static_cast<std::uint32_t>(iterations);
    info.password_hash.salt = row[4].as<std::string>();
    info.password_hash.derived_key = row[5].as<std::string>();
    if (info.password_hash.algorithm.empty() || info.password_hash.salt.empty() ||
        info.password_hash.derived_key.empty()) {
        return std::nullopt;
    }
    info.created_at_s = row[6].as<std::int64_t>(0);
    return info;
}

void log_query_error(std::string_view operation, const char* error) {
    common::Logger::instance()
        .error(common::LogDomain::Auth, "user postgres query failed")
        .field("operation", operation)
        .field("error", error != nullptr ? error : "unknown")
        .field("decision", "return_internal_error");
}

}  // namespace

bool check_user_schema_ready() {
    auto& pool = common::PostgresConnectionPool::instance();
    if (!pool.is_initialized()) {
        common::Logger::instance()
            .error(common::LogDomain::Auth, "user schema readiness check failed")
            .field("error", "connection_pool_not_initialized")
            .field("decision", "return_not_ready");
        return false;
    }

    const auto lease = common::acquire_connection_lease("check_user_schema_ready");
    if (!lease.has_value()) {
        return false;
    }

    try {
        pqxx::read_transaction tx(lease->connection());
        const pqxx::row row = tx.exec1(
            "SELECT to_regclass('public.users'), "
            "to_regclass('public.users_user_id_seq'), "
            "NULLIF((SELECT COUNT(*) = 7 FROM information_schema.columns "
            "WHERE table_schema = 'public' AND table_name = 'users' "
            "AND column_name IN ('user_id', 'username', 'password_hash_algorithm', "
            "'password_hash_iterations', 'password_hash_salt', 'password_hash_derived_key', "
            "'created_at_s')), FALSE)");
        const common::PostgresRowCheckStatus row_status = common::check_postgres_row_non_null_fields(row, 3);
        if (row_status == common::PostgresRowCheckStatus::InvalidSize) {
            common::Logger::instance()
                .error(common::LogDomain::Auth, "user schema readiness check failed")
                .field("error", "schema_check_result_invalid")
                .field("decision", "return_not_ready");
            return false;
        }
        if (row_status == common::PostgresRowCheckStatus::NullField) {
            common::Logger::instance()
                .error(common::LogDomain::Auth, "user schema readiness check failed")
                .field("error", "schema_object_missing")
                .field("decision", "return_not_ready");
            return false;
        }
        common::Logger::instance().info(common::LogDomain::Auth, "user schema ready");
        return true;
    } catch (const std::exception& e) {
        common::Logger::instance()
            .error(common::LogDomain::Auth, "user schema readiness check failed")
            .field("error", e.what())
            .field("decision", "return_not_ready");
        return false;
    }
}

UserAllocateIdResult allocate_user_id() {
    const auto lease = common::acquire_connection_lease("allocate_user_id");
    if (!lease.has_value()) {
        return {.status = UserAllocateIdStatus::InternalError};
    }

    try {
        pqxx::work tx(lease->connection());
        const pqxx::row row = tx.exec1("SELECT nextval('users_user_id_seq')");
        tx.commit();

        if (row.size() != 1 || row[0].is_null()) {
            return {.status = UserAllocateIdStatus::InternalError};
        }

        const auto user_id = row[0].as<std::int64_t>(0);
        if (user_id <= 0) {
            return {.status = UserAllocateIdStatus::InternalError};
        }

        return {.status = UserAllocateIdStatus::Allocated, .user_id = user_id};
    } catch (const std::exception& e) {
        log_query_error("allocate_user_id", e.what());
        return {.status = UserAllocateIdStatus::InternalError};
    }
}

UserCreateResult create_user(const UserInfo& info) {
    const auto lease = common::acquire_connection_lease("create_user");
    if (!lease.has_value()) {
        return UserCreateResult::InternalError;
    }
    if (info.user_id <= 0) {
        return UserCreateResult::InternalError;
    }

    try {
        pqxx::work tx(lease->connection());
        const pqxx::result inserted_rows = tx.exec_params(
            "INSERT INTO users(user_id, username, password_hash_algorithm, password_hash_iterations, "
            "password_hash_salt, password_hash_derived_key, created_at_s) "
            "VALUES($1::bigint, $2, $3, $4::bigint, $5, $6, $7::bigint) "
            "ON CONFLICT (username) DO NOTHING "
            "RETURNING user_id",
            std::to_string(info.user_id), info.username, info.password_hash.algorithm,
            std::to_string(info.password_hash.iterations), info.password_hash.salt, info.password_hash.derived_key,
            std::to_string(info.created_at_s));
        tx.commit();
        if (inserted_rows.empty()) {
            return UserCreateResult::DuplicateUsername;
        }
        return UserCreateResult::Created;
    } catch (const pqxx::sql_error& e) {
        common::Logger::instance()
            .error(common::LogDomain::Auth, "user postgres query failed")
            .field("operation", "create_user")
            .field("sql_state", e.sqlstate())
            .field("error", e.what())
            .field("decision", "return_internal_error");
        return UserCreateResult::InternalError;
    } catch (const std::exception& e) {
        log_query_error("create_user", e.what());
        return UserCreateResult::InternalError;
    }
}

UserFindResult find_user_by_username(std::string_view username) {
    const auto lease = common::acquire_connection_lease("find_by_username");
    if (!lease.has_value()) {
        return {.status = UserFindStatus::InternalError, .info = {}};
    }

    try {
        pqxx::read_transaction tx(lease->connection());
        const pqxx::result rows = tx.exec_params(
            "SELECT user_id, username, password_hash_algorithm, password_hash_iterations, password_hash_salt, "
            "password_hash_derived_key, created_at_s FROM users WHERE username = $1 LIMIT 1",
            std::string(username));
        if (rows.empty()) {
            return {.status = UserFindStatus::NotFound, .info = {}};
        }

        const std::optional<UserInfo> info = parse_user_row(rows.front());
        if (!info.has_value()) {
            return {.status = UserFindStatus::InternalError, .info = {}};
        }
        return {.status = UserFindStatus::Found, .info = *info};
    } catch (const std::exception& e) {
        log_query_error("find_by_username", e.what());
        return {.status = UserFindStatus::InternalError, .info = {}};
    }
}

UserFindResult find_user_by_id(std::int64_t user_id) {
    if (user_id <= 0) {
        return {.status = UserFindStatus::NotFound, .info = {}};
    }
    const auto lease = common::acquire_connection_lease("find_by_user_id");
    if (!lease.has_value()) {
        return {.status = UserFindStatus::InternalError, .info = {}};
    }

    try {
        pqxx::read_transaction tx(lease->connection());
        const pqxx::result rows = tx.exec_params(
            "SELECT user_id, username, password_hash_algorithm, password_hash_iterations, password_hash_salt, "
            "password_hash_derived_key, created_at_s FROM users WHERE user_id = $1::bigint LIMIT 1",
            std::to_string(user_id));
        if (rows.empty()) {
            return {.status = UserFindStatus::NotFound, .info = {}};
        }

        const std::optional<UserInfo> info = parse_user_row(rows.front());
        if (!info.has_value()) {
            return {.status = UserFindStatus::InternalError, .info = {}};
        }
        return {.status = UserFindStatus::Found, .info = *info};
    } catch (const std::exception& e) {
        log_query_error("find_by_user_id", e.what());
        return {.status = UserFindStatus::InternalError, .info = {}};
    }
}

}  // namespace nebula::auth
