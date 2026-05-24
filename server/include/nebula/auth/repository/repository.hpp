#ifndef NEBULA_AUTH_REPOSITORY_REPOSITORY_HPP
#define NEBULA_AUTH_REPOSITORY_REPOSITORY_HPP

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string_view>

#include "nebula/auth/repository/types.hpp"
#include "nebula/database/connection_pool.hpp"

namespace nebula::auth {

class AuthRepository {
public:
    explicit AuthRepository(std::shared_ptr<database::ConnectionPool> database_pool);

    [[nodiscard]] bool check_schema_ready();

    [[nodiscard]] std::expected<UserAuthRecord, UserCreateError> create_user(std::string_view username,
                                                                             const PasswordHashValue& password_hash,
                                                                             std::int64_t now_s);

    [[nodiscard]] std::expected<UserAuthRecord, UserFindError> find_user_by_username(std::string_view username);
    [[nodiscard]] std::expected<UserAuthRecord, UserFindError> find_user_by_id(std::int64_t user_id);
    [[nodiscard]] std::expected<UserAuthRecord, UserPasswordUpdateError> update_password_hash(
        std::int64_t user_id, const PasswordHashValue& password_hash, std::int64_t token_version);
    [[nodiscard]] std::optional<UserListPage> list_users(std::int64_t limit, std::int64_t offset);
    [[nodiscard]] std::expected<UserProfile, UserRoleStatusUpdateError> update_user(std::int64_t user_id,
                                                                                    std::optional<UserRole> role,
                                                                                    std::optional<UserStatus> status);

private:
    std::shared_ptr<database::ConnectionPool> database_pool_;
};

}  // namespace nebula::auth

#endif  // NEBULA_AUTH_REPOSITORY_REPOSITORY_HPP
