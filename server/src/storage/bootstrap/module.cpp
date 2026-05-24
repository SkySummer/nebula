#include "nebula/storage/bootstrap/module.hpp"

#include <chrono>
#include <filesystem>
#include <memory>

#include "nebula/common/log/logger.hpp"
#include "nebula/storage/http/routes.hpp"

namespace nebula::storage {

namespace {

void log_storage_module_init_failed(std::string_view error) {
    common::Logger::instance()
        .fatal("storage module init failed")
        .field("error", error)
        .field("decision", "return_null");
}

}  // namespace

StorageModule::StorageModule(std::shared_ptr<ObjectStore> object_store, std::shared_ptr<StorageRepository> repository,
                             std::shared_ptr<StorageService> service)
    : object_store_(std::move(object_store)), repository_(std::move(repository)), service_(std::move(service)) {}

std::unique_ptr<StorageModule> StorageModule::create(Params params) noexcept {
    if (params.config == nullptr) {
        log_storage_module_init_failed("config_missing");
        return nullptr;
    }
    if (params.limits == nullptr) {
        log_storage_module_init_failed("limits_missing");
        return nullptr;
    }
    if (params.database_pool == nullptr) {
        log_storage_module_init_failed("database_pool_missing");
        return nullptr;
    }
    if (params.router == nullptr) {
        log_storage_module_init_failed("router_missing");
        return nullptr;
    }

    try {
        auto repository = std::make_shared<StorageRepository>(std::move(params.database_pool));
        if (!repository->check_schema_ready()) {
            log_storage_module_init_failed("storage_repository_not_ready");
            return nullptr;
        }

        const std::filesystem::path root_dir = params.config->root_dir;
        StorageRuntimeConfig route_config{
            .root_dir = root_dir,
            .temp_dir = root_dir / "temp",
            .objects_dir = root_dir / "objects",
            .upload_session_ttl = std::chrono::seconds(params.config->upload_session_ttl_s),
            .download_ticket_ttl = std::chrono::seconds(params.config->download_ticket_ttl_s),
            .max_body_bytes = params.limits->max_body_bytes,
            .max_file_bytes = params.config->max_file_bytes,
        };

        auto object_store = std::make_shared<ObjectStore>(route_config);
        if (!object_store->ensure_root_dirs()) {
            log_storage_module_init_failed("storage_object_store_not_ready");
            return nullptr;
        }

        auto service = std::make_shared<StorageService>(repository, object_store, route_config);
        if (!register_storage_routes(params.router, service)) {
            log_storage_module_init_failed("register_storage_routes_failed");
            return nullptr;
        }

        return std::unique_ptr<StorageModule>(
            new StorageModule(std::move(object_store), std::move(repository), std::move(service)));
    } catch (const std::exception& e) {
        log_storage_module_init_failed(e.what());
        return nullptr;
    } catch (...) {
        log_storage_module_init_failed("unknown");
        return nullptr;
    }
}

}  // namespace nebula::storage
