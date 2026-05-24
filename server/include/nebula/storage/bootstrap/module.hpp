#ifndef NEBULA_STORAGE_BOOTSTRAP_MODULE_HPP
#define NEBULA_STORAGE_BOOTSTRAP_MODULE_HPP

#include <memory>

#include "nebula/database/connection_pool.hpp"
#include "nebula/http/config/http_limits_config.hpp"
#include "nebula/http/routing/router.hpp"
#include "nebula/storage/application/service.hpp"
#include "nebula/storage/bootstrap/config.hpp"
#include "nebula/storage/repository/repository.hpp"

namespace nebula::storage {

class StorageModule {
public:
    struct Params {
        const StorageConfig* config;
        const http::HttpLimitsConfig* limits;
        std::shared_ptr<database::ConnectionPool> database_pool;
        std::shared_ptr<http::Router> router;
    };

    [[nodiscard]] static std::unique_ptr<StorageModule> create(Params params) noexcept;

private:
    StorageModule(std::shared_ptr<ObjectStore> object_store, std::shared_ptr<StorageRepository> repository,
                  std::shared_ptr<StorageService> service);

    std::shared_ptr<ObjectStore> object_store_;
    std::shared_ptr<StorageRepository> repository_;
    std::shared_ptr<StorageService> service_;
};

}  // namespace nebula::storage

#endif  // NEBULA_STORAGE_BOOTSTRAP_MODULE_HPP
