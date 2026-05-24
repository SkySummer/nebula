#include "nebula/storage/repository/file_nodes_executor.hpp"

#include <string>

#include "nebula/storage/repository/file_nodes_sql.hpp"

namespace nebula::storage {

namespace {

[[nodiscard]] std::string user_root_path(std::int64_t user_id) {
    return std::format("/users/{}", user_id);
}

[[nodiscard]] std::string user_storage_prefix(std::int64_t user_id) {
    return std::format("{}/", user_root_path(user_id));
}

[[nodiscard]] std::string escape_like_literal(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char ch : text) {
        if (ch == '\\' || ch == '%' || ch == '_') {
            escaped.push_back('\\');
        }
        escaped.push_back(ch);
    }
    return escaped;
}

[[nodiscard]] std::string build_prefix_like_pattern(std::string_view prefix) {
    std::string pattern = escape_like_literal(prefix);
    pattern.push_back('%');
    return pattern;
}

[[nodiscard]] std::string_view directory_list_order_by_clause(const DirectoryListOptions& options) {
    switch (options.sort_by) {
        case DirectoryListSortBy::Name:
            return options.sort_direction == DirectoryListSortDirection::Desc
                       ? " CASE WHEN node_type = 'directory' THEN 0 ELSE 1 END ASC, path DESC"
                       : " CASE WHEN node_type = 'directory' THEN 0 ELSE 1 END ASC, path ASC";
        case DirectoryListSortBy::UpdatedAt:
            return options.sort_direction == DirectoryListSortDirection::Desc
                       ? " CASE WHEN node_type = 'directory' THEN 0 ELSE 1 END ASC, updated_at_s DESC, path ASC"
                       : " CASE WHEN node_type = 'directory' THEN 0 ELSE 1 END ASC, updated_at_s ASC, path ASC";
    }
    std::unreachable();
}

[[nodiscard]] std::string build_list_directory_children_sql(const DirectoryListOptions& options) {
    return std::format("{}{}", kListDirectoryChildrenSqlPrefix, directory_list_order_by_clause(options));
}

}  // namespace

void execute_ensure_user_root_directory(pqxx::work& tx, std::int64_t user_id, std::int64_t now_s) {
    tx.exec_params(std::string(kEnsureUserRootDirectorySql), user_root_path(user_id), now_s);
}

pqxx::result execute_find_node_type_for_update(pqxx::work& tx, std::string_view path) {
    return tx.exec_params(std::string(kFindStorageNodeTypeForUpdateSql), std::string(path));
}

pqxx::result execute_find_storage_node_descendant(pqxx::work& tx, std::string_view path) {
    return tx.exec_params(std::string(kFindStorageNodeDescendantSql), build_prefix_like_pattern(path));
}

pqxx::result execute_find_user_quota_bytes(pqxx::transaction_base& tx, std::int64_t user_id) {
    return tx.exec_params(std::string(kFindUserQuotaBytesSql), user_id);
}

pqxx::row execute_sum_user_file_bytes(pqxx::transaction_base& tx, std::int64_t user_id) {
    return tx.exec_params1(std::string(kSumUserFileBytesSql), build_prefix_like_pattern(user_storage_prefix(user_id)));
}

pqxx::result execute_find_existing_file_target_for_update(pqxx::work& tx, std::string_view path) {
    return tx.exec_params(std::string(kFindExistingFileTargetForUpdateSql), std::string(path));
}

pqxx::result execute_find_file_node(pqxx::read_transaction& tx, std::string_view path) {
    return tx.exec_params(std::string(kFindFileNodeSql), std::string(path));
}

void execute_insert_directory_node(pqxx::work& tx, std::string_view scoped_path, std::int64_t now_s) {
    tx.exec_params(std::string(kInsertDirectoryNodeSql), std::string(scoped_path), now_s);
}

pqxx::result execute_list_directory_children(pqxx::work& tx, std::string_view scoped_path,
                                             const DirectoryListOptions& options) {
    return tx.exec_params(build_list_directory_children_sql(options),
                          build_prefix_like_pattern(std::format("{}/", scoped_path)));
}

pqxx::result execute_list_recent_files(pqxx::work& tx, std::int64_t user_id, std::int64_t limit) {
    return tx.exec_params(std::string(kListRecentFilesSql), build_prefix_like_pattern(user_storage_prefix(user_id)),
                          limit);
}

pqxx::result execute_list_storage_usage_files(pqxx::work& tx, std::int64_t user_id) {
    return tx.exec_params(std::string(kListStorageUsageFilesSql),
                          build_prefix_like_pattern(user_storage_prefix(user_id)));
}

void execute_upsert_storage_object(pqxx::work& tx, std::string_view sha256, std::int64_t size_bytes,
                                   std::string_view object_rel_path, std::int64_t now_s) {
    tx.exec_params(std::string(kUpsertStorageObjectSql), std::string(sha256), size_bytes, std::string(object_rel_path),
                   now_s);
}

void execute_insert_file_node(pqxx::work& tx, std::string_view path, std::string_view sha256, std::int64_t size_bytes,
                              std::int64_t now_s) {
    tx.exec_params(std::string(kInsertFileNodeSql), std::string(path), std::string(sha256), size_bytes, now_s);
}

void execute_update_file_node_with_object(pqxx::work& tx, std::string_view path, std::string_view sha256,
                                          std::int64_t size_bytes, std::int64_t now_s) {
    tx.exec_params(std::string(kUpdateFileNodeWithObjectSql), std::string(path), std::string(sha256), size_bytes,
                   now_s);
}

void execute_update_file_node_size(pqxx::work& tx, std::string_view path, std::int64_t size_bytes, std::int64_t now_s) {
    tx.exec_params(std::string(kUpdateFileNodeSizeSql), std::string(path), size_bytes, now_s);
}

pqxx::result execute_decrement_storage_object_ref_count(pqxx::work& tx, std::string_view sha256, std::int64_t now_s) {
    return tx.exec_params(std::string(kDecrementStorageObjectRefCountSql), std::string(sha256), now_s);
}

pqxx::result execute_find_node_for_delete(pqxx::work& tx, std::string_view scoped_path) {
    return tx.exec_params(std::string(kFindNodeForDeleteSql), std::string(scoped_path));
}

void execute_delete_node(pqxx::work& tx, std::string_view scoped_path) {
    tx.exec_params(std::string(kDeleteNodeSql), std::string(scoped_path));
}

}  // namespace nebula::storage
