#include "nebula/storage/domain/permissions.hpp"

namespace nebula::storage {

bool can_run_global_storage_gc(auth::UserRole role) noexcept {
    return role == auth::UserRole::Owner || role == auth::UserRole::Admin;
}

}  // namespace nebula::storage
