#ifndef NEBULA_STORAGE_STORAGE_HTTP_HPP
#define NEBULA_STORAGE_STORAGE_HTTP_HPP

#include <memory>

#include "nebula/http/router.hpp"
#include "nebula/server/server_config.hpp"

namespace nebula::storage {

[[nodiscard]] bool register_storage_routes(const server::ServerConfig& config,
                                           const std::shared_ptr<http::Router>& router);

}  // namespace nebula::storage

#endif  // NEBULA_STORAGE_STORAGE_HTTP_HPP
