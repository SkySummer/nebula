#ifndef NEBULA_STORAGE_REPOSITORY_GC_ROW_PARSER_HPP
#define NEBULA_STORAGE_REPOSITORY_GC_ROW_PARSER_HPP

#include <cstdint>
#include <optional>
#include <pqxx/pqxx>
#include <string>

#include "nebula/storage/repository/types.hpp"

namespace nebula::storage {

struct UnreferencedObjectCleanupRow {
    std::string object_rel_path;
    std::int64_t ref_count = 0;
    bool has_file_node_reference = false;
};

std::optional<GcExpiredUploadSession> parse_gc_expired_upload_session_row(const pqxx::row& row);

std::optional<std::string> parse_string_value_row(const pqxx::row& row);

std::optional<GcOrphanObject> parse_gc_orphan_object_row(const pqxx::row& row);

std::optional<UnreferencedObjectCleanupRow> parse_unreferenced_object_cleanup_row(const pqxx::row& row);

}  // namespace nebula::storage

#endif  // NEBULA_STORAGE_REPOSITORY_GC_ROW_PARSER_HPP
