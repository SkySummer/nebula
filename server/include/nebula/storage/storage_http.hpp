#ifndef NEBULA_STORAGE_STORAGE_HTTP_HPP
#define NEBULA_STORAGE_STORAGE_HTTP_HPP

#include <memory>

#include "nebula/app/server_config.hpp"
#include "nebula/http/router.hpp"

namespace nebula::storage {

[[nodiscard]] bool register_storage_routes(const app::ServerConfig& config,
                                           const std::shared_ptr<http::Router>& router);

}  // namespace nebula::storage

#endif  // NEBULA_STORAGE_STORAGE_HTTP_HPP
