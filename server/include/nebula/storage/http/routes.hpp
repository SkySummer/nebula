#ifndef NEBULA_STORAGE_HTTP_ROUTES_HPP
#define NEBULA_STORAGE_HTTP_ROUTES_HPP

#include <memory>

#include "nebula/http/routing/router.hpp"

namespace nebula::storage {

class StorageService;

[[nodiscard]] bool register_storage_routes(const std::shared_ptr<http::Router>& router,
                                           const std::shared_ptr<StorageService>& service);

}  // namespace nebula::storage

#endif  // NEBULA_STORAGE_HTTP_ROUTES_HPP
