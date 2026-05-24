#ifndef NEBULA_AUTH_DOMAIN_PASSWORD_HASH_HPP
#define NEBULA_AUTH_DOMAIN_PASSWORD_HASH_HPP

#include <cstdint>
#include <string>

namespace nebula::auth {

struct PasswordHashValue {
    std::string algorithm;
    std::uint32_t iterations = 0;
    std::string salt;
    std::string derived_key;
};

}  // namespace nebula::auth

#endif  // NEBULA_AUTH_DOMAIN_PASSWORD_HASH_HPP
