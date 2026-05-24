#include "nebula/storage/infra/object_store.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include <openssl/evp.h>

#include "nebula/common/base/hex.hpp"
#include "nebula/common/log/logger.hpp"
#include "nebula/common/platform/file_io.hpp"
#include "nebula/common/security/crypto.hpp"
#include "nebula/storage/domain/path.hpp"

namespace nebula::storage {

namespace {

[[nodiscard]] bool would_exceed_file_size_limit(std::int64_t committed_size_bytes, std::size_t append_size,
                                                std::int64_t max_file_bytes) {
    if (committed_size_bytes < 0 || max_file_bytes <= 0) {
        return true;
    }
    if (committed_size_bytes > max_file_bytes) {
        return true;
    }
    if (!std::in_range<std::int64_t>(append_size)) {
        return true;
    }

    const auto append_size_i64 = static_cast<std::int64_t>(append_size);
    return append_size_i64 > (max_file_bytes - committed_size_bytes);
}

[[nodiscard]] bool is_regular_file_older_than(const std::filesystem::directory_entry& entry,
                                              std::chrono::seconds min_age_s) {
    std::error_code ec;
    const bool is_regular_file = entry.is_regular_file(ec);
    if (ec || !is_regular_file) {
        return false;
    }

    ec.clear();
    const auto last_write_time = entry.last_write_time(ec);
    if (ec) {
        return false;
    }
    const auto file_age_s = std::chrono::duration_cast<std::chrono::seconds>(
        std::filesystem::file_time_type::clock::now() - last_write_time);
    return file_age_s >= min_age_s;
}

}  // namespace

ObjectStore::ObjectStore(StorageRuntimeConfig config) : config_(std::move(config)) {}

bool ObjectStore::ensure_root_dirs() const {
    std::error_code ec;
    std::filesystem::create_directories(config_.temp_dir, ec);
    if (ec) {
        common::Logger::instance()
            .error("storage directory create failed")
            .field("path", config_.temp_dir)
            .field("errno", ec.value())
            .field("error", ec.message())
            .field("decision", "stop_init");
        return false;
    }

    ec.clear();
    std::filesystem::create_directories(config_.objects_dir, ec);
    if (ec) {
        common::Logger::instance()
            .error("storage directory create failed")
            .field("path", config_.objects_dir)
            .field("errno", ec.value())
            .field("error", ec.message())
            .field("decision", "stop_init");
        return false;
    }

    return true;
}

bool ObjectStore::create_empty_temp_file(const std::filesystem::path& temp_path) {
    std::error_code ec;
    std::filesystem::create_directories(temp_path.parent_path(), ec);
    if (ec) {
        common::Logger::instance()
            .error("temp directory create failed")
            .field("path", temp_path.parent_path())
            .field("errno", ec.value())
            .field("error", ec.message())
            .field("decision", "return_failed");
        return false;
    }

    const auto write_result = common::write_binary_file(temp_path, std::span<const std::byte>{});
    if (!write_result.has_value()) {
        common::Logger::instance()
            .error("temp file create failed")
            .field("path", temp_path)
            .field("error", common::to_string(write_result.error()))
            .field("decision", "return_failed");
        return false;
    }
    return true;
}

AppendTempChunkResult ObjectStore::append_temp_chunk(const std::filesystem::path& temp_path, std::string_view body,
                                                     std::int64_t committed_size_bytes) const {
    if (!std::in_range<std::int64_t>(body.size())) {
        return {};
    }
    if (would_exceed_file_size_limit(committed_size_bytes, body.size(), config_.max_file_bytes)) {
        return {.status = AppendTempChunkStatus::FileTooLarge, .bytes_written = 0};
    }
    if (!restore_temp_size(temp_path, committed_size_bytes)) {
        return {};
    }

    const auto bytes_written = static_cast<std::int64_t>(body.size());
    std::ofstream out(temp_path, std::ios::binary | std::ios::app);
    if (!out.is_open()) {
        common::Logger::instance()
            .error("temp chunk append failed")
            .field("path", temp_path)
            .field("error", "open_failed")
            .field("decision", "return_failed");
        return {};
    }
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
    out.flush();
    const bool write_ok = out.good();
    out.close();

    std::error_code ec;
    const std::uintmax_t actual_size = std::filesystem::file_size(temp_path, ec);
    const std::uintmax_t expected_size =
        static_cast<std::uintmax_t>(committed_size_bytes) + static_cast<std::uintmax_t>(bytes_written);
    if (!write_ok || ec || actual_size != expected_size) {
        [[maybe_unused]] const bool restored = restore_temp_size(temp_path, committed_size_bytes);
        if (ec) {
            common::Logger::instance()
                .error("temp chunk append failed")
                .field("path", temp_path)
                .field("errno", ec.value())
                .field("error", ec.message())
                .field("decision", "restore_temp_size");
        } else {
            common::Logger::instance()
                .error("temp chunk append failed")
                .field("path", temp_path)
                .field("error", "write_verification_failed")
                .field("decision", "restore_temp_size");
        }
        return {};
    }

    return {.status = AppendTempChunkStatus::Appended, .bytes_written = bytes_written};
}

bool ObjectStore::restore_temp_size(const std::filesystem::path& temp_path, std::int64_t committed_size_bytes) {
    if (committed_size_bytes < 0) {
        common::Logger::instance()
            .error("temp file size restore failed")
            .field("path", temp_path)
            .field("error", "invalid_committed_size")
            .field("decision", "return_failed");
        return false;
    }

    std::error_code ec;
    const std::uintmax_t current_size = std::filesystem::file_size(temp_path, ec);
    if (ec) {
        common::Logger::instance()
            .error("temp file size restore failed")
            .field("path", temp_path)
            .field("errno", ec.value())
            .field("error", ec.message())
            .field("decision", "return_failed");
        return false;
    }
    if (std::cmp_less(current_size, committed_size_bytes)) {
        common::Logger::instance()
            .error("temp file size restore failed")
            .field("path", temp_path)
            .field("error", "committed_size_exceeds_file_size")
            .field("decision", "return_failed");
        return false;
    }
    if (std::cmp_equal(current_size, committed_size_bytes)) {
        return true;
    }

    std::filesystem::resize_file(temp_path, static_cast<std::uintmax_t>(committed_size_bytes), ec);
    if (ec) {
        common::Logger::instance()
            .error("temp file size restore failed")
            .field("path", temp_path)
            .field("errno", ec.value())
            .field("error", ec.message())
            .field("decision", "return_failed");
    }
    return !ec;
}

std::optional<HashAndSize> ObjectStore::hash_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        common::Logger::instance()
            .error("file hash failed")
            .field("path", path)
            .field("error", "open_failed")
            .field("decision", "return_failed");
        return std::nullopt;
    }

    EVP_MD_CTX* ctx = ::EVP_MD_CTX_new();
    if (ctx == nullptr) {
        common::Logger::instance()
            .error("file hash failed")
            .field("path", path)
            .field("error", "digest_context_alloc_failed")
            .field("decision", "return_failed");
        return std::nullopt;
    }
    const bool init_ok = ::EVP_DigestInit_ex(ctx, ::EVP_sha256(), nullptr) == 1;
    if (!init_ok) {
        ::EVP_MD_CTX_free(ctx);
        common::Logger::instance()
            .error("file hash failed")
            .field("path", path)
            .field("error", "digest_init_failed")
            .field("decision", "return_failed");
        return std::nullopt;
    }

    std::array<char, std::size_t{64} * 1024U> buffer{};
    std::int64_t total = 0;
    while (true) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize read_n = in.gcount();
        if (read_n > 0) {
            if (::EVP_DigestUpdate(ctx, buffer.data(), static_cast<std::size_t>(read_n)) != 1) {
                ::EVP_MD_CTX_free(ctx);
                common::Logger::instance()
                    .error("file hash failed")
                    .field("path", path)
                    .field("error", "digest_update_failed")
                    .field("decision", "return_failed");
                return std::nullopt;
            }
            total += static_cast<std::int64_t>(read_n);
        }
        if (in.eof()) {
            break;
        }
        if (!in.good()) {
            ::EVP_MD_CTX_free(ctx);
            common::Logger::instance()
                .error("file hash failed")
                .field("path", path)
                .field("error", "read_failed")
                .field("decision", "return_failed");
            return std::nullopt;
        }
    }

    unsigned int digest_len = 0;
    std::array<std::uint8_t, EVP_MAX_MD_SIZE> digest{};
    if (::EVP_DigestFinal_ex(ctx, digest.data(), &digest_len) != 1) {
        ::EVP_MD_CTX_free(ctx);
        common::Logger::instance()
            .error("file hash failed")
            .field("path", path)
            .field("error", "digest_final_failed")
            .field("decision", "return_failed");
        return std::nullopt;
    }
    ::EVP_MD_CTX_free(ctx);

    HashAndSize result;
    result.sha256_hex = common::bytes_to_hex({digest.data(), digest_len});
    result.size_bytes = total;
    return result;
}

PublishObjectStatus ObjectStore::publish_object(const std::filesystem::path& temp_path,
                                                const std::filesystem::path& object_path,
                                                std::string_view expected_sha256, std::int64_t expected_size_bytes) {
    std::error_code ec;
    std::filesystem::create_directories(object_path.parent_path(), ec);
    if (ec) {
        common::Logger::instance()
            .error("object directory create failed")
            .field("path", object_path.parent_path())
            .field("errno", ec.value())
            .field("error", ec.message())
            .field("decision", "return_failed");
        return PublishObjectStatus::Failed;
    }

    ec.clear();
    if (std::filesystem::exists(object_path, ec)) {
        if (ec) {
            common::Logger::instance()
                .error("object file status check failed")
                .field("path", object_path)
                .field("errno", ec.value())
                .field("error", ec.message())
                .field("decision", "return_failed");
            return PublishObjectStatus::Failed;
        }

        const std::optional<HashAndSize> existing_hash = hash_file(object_path);
        if (existing_hash.has_value() && existing_hash->sha256_hex == expected_sha256 &&
            existing_hash->size_bytes == expected_size_bytes) {
            return PublishObjectStatus::AlreadyExists;
        }

        common::Logger::instance()
            .error("object file verify failed")
            .field("path", object_path)
            .field("error", "sha256_or_size_mismatch")
            .field("decision", "return_failed");
        return PublishObjectStatus::Failed;
    }

    const std::optional<std::string> staging_id = common::generate_random_hex_token_128();
    if (!staging_id.has_value()) {
        return PublishObjectStatus::Failed;
    }
    const std::filesystem::path staging_path = std::format("{}.tmp.{}", object_path.generic_string(), *staging_id);

    ec.clear();
    std::filesystem::copy_file(temp_path, staging_path, std::filesystem::copy_options::none, ec);
    if (ec) {
        delete_file_if_exists(staging_path);
        common::Logger::instance()
            .error("object staging copy failed")
            .field("path", staging_path)
            .field("errno", ec.value())
            .field("error", ec.message())
            .field("decision", "return_failed");
        return PublishObjectStatus::Failed;
    }

    const std::optional<HashAndSize> staging_hash = hash_file(staging_path);
    if (!staging_hash.has_value() || staging_hash->sha256_hex != expected_sha256 ||
        staging_hash->size_bytes != expected_size_bytes) {
        delete_file_if_exists(staging_path);
        common::Logger::instance()
            .error("object staging verify failed")
            .field("path", staging_path)
            .field("error", "sha256_or_size_mismatch")
            .field("decision", "return_failed");
        return PublishObjectStatus::Failed;
    }

    std::filesystem::create_hard_link(staging_path, object_path, ec);
    const std::error_code link_ec = ec;
    delete_file_if_exists(staging_path);
    if (!ec) {
        return PublishObjectStatus::Created;
    }

    ec.clear();
    if (std::filesystem::exists(object_path, ec) && !ec) {
        const std::optional<HashAndSize> existing_hash = hash_file(object_path);
        if (existing_hash.has_value() && existing_hash->sha256_hex == expected_sha256 &&
            existing_hash->size_bytes == expected_size_bytes) {
            return PublishObjectStatus::AlreadyExists;
        }
        common::Logger::instance()
            .error("object publish failed")
            .field("path", object_path)
            .field("error", "sha256_or_size_mismatch")
            .field("decision", "return_failed");
        return PublishObjectStatus::Failed;
    }
    if (ec) {
        common::Logger::instance()
            .error("object file status check failed")
            .field("path", object_path)
            .field("errno", ec.value())
            .field("error", ec.message())
            .field("decision", "return_failed");
    } else {
        common::Logger::instance()
            .error("object publish failed")
            .field("path", object_path)
            .field("errno", link_ec.value())
            .field("error", link_ec.message())
            .field("decision", "return_failed");
    }

    return PublishObjectStatus::Failed;
}

std::expected<std::string, common::ReadFileError> ObjectStore::read_file(const std::filesystem::path& path) const {
    if (config_.max_file_bytes <= 0) {
        return std::unexpected(common::ReadFileError::TooLarge);
    }

    auto body = common::read_file(path, static_cast<std::uintmax_t>(config_.max_file_bytes));
    if (!body.has_value()) {
        if (body.error() != common::ReadFileError::TooLarge) {
            common::Logger::instance()
                .error("file read failed")
                .field("path", path)
                .field("error", common::to_string(body.error()))
                .field("decision", "return_failed");
        }
        return std::unexpected(body.error());
    }

    return body;
}

std::vector<std::filesystem::path> ObjectStore::scan_orphan_objects(
    const std::unordered_set<std::string>& object_rel_paths_in_db, std::chrono::seconds min_age_s) const {
    std::vector<std::filesystem::path> orphan_files;

    std::error_code ec;
    if (!std::filesystem::exists(config_.objects_dir, ec) || ec) {
        return orphan_files;
    }

    std::filesystem::recursive_directory_iterator iter(config_.objects_dir,
                                                       std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    while (!ec && iter != end) {
        const std::filesystem::directory_entry entry = *iter;
        if (is_regular_file_older_than(entry, min_age_s)) {
            ec.clear();
            const std::filesystem::path rel_path = std::filesystem::relative(entry.path(), config_.root_dir, ec);
            if (!ec) {
                const std::string rel_path_str = rel_path.generic_string();
                if (!object_rel_paths_in_db.contains(rel_path_str)) {
                    orphan_files.push_back(entry.path());
                }
            }
        }

        ec.clear();
        iter.increment(ec);
    }

    return orphan_files;
}

std::vector<std::filesystem::path> ObjectStore::scan_orphan_temps(
    const std::unordered_set<std::string>& active_temp_rel_paths, std::chrono::seconds min_age_s) const {
    std::vector<std::filesystem::path> orphan_files;

    std::error_code ec;
    if (!std::filesystem::exists(config_.temp_dir, ec) || ec) {
        return orphan_files;
    }

    std::filesystem::recursive_directory_iterator iter(config_.temp_dir,
                                                       std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    while (!ec && iter != end) {
        const std::filesystem::directory_entry entry = *iter;
        if (entry.path().extension() == ".part" && is_regular_file_older_than(entry, min_age_s)) {
            ec.clear();
            const std::filesystem::path rel_path = std::filesystem::relative(entry.path(), config_.root_dir, ec);
            if (!ec && !active_temp_rel_paths.contains(rel_path.generic_string())) {
                orphan_files.push_back(entry.path());
            }
        }

        ec.clear();
        iter.increment(ec);
    }

    return orphan_files;
}

bool ObjectStore::delete_object_path(const std::filesystem::path& object_path) const {
    return delete_path(object_path, config_.objects_dir);
}

bool ObjectStore::delete_temp_path(const std::filesystem::path& temp_path) const {
    return delete_path(temp_path, config_.temp_dir);
}

bool ObjectStore::delete_path(const std::filesystem::path& path, const std::filesystem::path& cleanup_root) {
    if (!delete_file_if_exists(path)) {
        return false;
    }

    try_cleanup_empty_parents(path, cleanup_root);
    return true;
}

}  // namespace nebula::storage
