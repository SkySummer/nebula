#include "nebula/storage/bootstrap/config.hpp"

#include <limits>

#include "nebula/common/log/logger.hpp"

namespace nebula::storage {

bool StorageConfig::validate() const {
    bool ok = true;
    if (root_dir.empty()) {
        common::Logger::instance()
            .error("storage config value invalid")
            .field("key", "root_dir")
            .field("error", "empty_value");
        ok = false;
    }
    if (upload_session_ttl_s <= 0 || upload_session_ttl_s > kMaxStorageUploadSessionTtlSeconds) {
        common::Logger::instance()
            .error("storage config value out of range")
            .field("key", "upload_session_ttl_s")
            .field("value", upload_session_ttl_s)
            .field("min_value", 1)
            .field("max_value", kMaxStorageUploadSessionTtlSeconds);
        ok = false;
    }
    if (download_ticket_ttl_s <= 0 || download_ticket_ttl_s > kMaxStorageDownloadTicketTtlSeconds) {
        common::Logger::instance()
            .error("storage config value out of range")
            .field("key", "download_ticket_ttl_s")
            .field("value", download_ticket_ttl_s)
            .field("min_value", 1)
            .field("max_value", kMaxStorageDownloadTicketTtlSeconds);
        ok = false;
    }
    if (max_file_bytes <= 0) {
        common::Logger::instance()
            .error("storage config value out of range")
            .field("key", "max_file_bytes")
            .field("value", max_file_bytes)
            .field("min_value", 1)
            .field("max_value", std::numeric_limits<std::int64_t>::max());
        ok = false;
    }
    return ok;
}

}  // namespace nebula::storage
