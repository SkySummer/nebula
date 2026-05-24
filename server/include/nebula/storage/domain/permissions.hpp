#ifndef NEBULA_STORAGE_DOMAIN_PERMISSIONS_HPP
#define NEBULA_STORAGE_DOMAIN_PERMISSIONS_HPP

#include "nebula/auth/domain/user.hpp"

namespace nebula::storage {

[[nodiscard]] bool can_run_global_storage_gc(auth::UserRole role) noexcept;

}  // namespace nebula::storage

#endif  // NEBULA_STORAGE_DOMAIN_PERMISSIONS_HPP
