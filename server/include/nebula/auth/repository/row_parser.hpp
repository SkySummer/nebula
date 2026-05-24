#ifndef NEBULA_AUTH_REPOSITORY_ROW_PARSER_HPP
#define NEBULA_AUTH_REPOSITORY_ROW_PARSER_HPP

#include <optional>
#include <pqxx/pqxx>

#include "nebula/auth/domain/user.hpp"

namespace nebula::auth {

std::optional<UserAuthRecord> parse_user_auth_row(const pqxx::row& row);

std::optional<UserProfile> parse_user_profile_row(const pqxx::row& row);

}  // namespace nebula::auth

#endif  // NEBULA_AUTH_REPOSITORY_ROW_PARSER_HPP
