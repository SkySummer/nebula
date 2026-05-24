#include "nebula/storage/infra/object_store.hpp"

#include <chrono>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "nebula/common/base/hex.hpp"
#include "nebula_tests/common.hpp"

namespace {

using namespace nebula;

storage::StorageRuntimeConfig make_route_config(const std::filesystem::path& root_dir) {
    return {
        .root_dir = root_dir,
        .temp_dir = root_dir / "temp",
        .objects_dir = root_dir / "objects",
        .upload_session_ttl = std::chrono::seconds{86400},
        .download_ticket_ttl = std::chrono::seconds{120},
        .max_body_bytes = std::size_t{1024} * 1024U,
        .max_file_bytes = 64LL * 1024 * 1024,
    };
}

void test_object_store_ensures_root_dirs() {
    const test::TempDir temp_dir("nebula-object-store-dirs");
    const storage::StorageRuntimeConfig route_config = make_route_config(temp_dir.path() / "files");
    const storage::ObjectStore store(route_config);

    test::expect_true(store.ensure_root_dirs(), "object store should create root directories");
    test::expect_true(std::filesystem::is_directory(route_config.temp_dir), "object store should create temp dir");
    test::expect_true(std::filesystem::is_directory(route_config.objects_dir),
                      "object store should create objects dir");
}

void test_object_store_appends_and_restores_temp_file() {
    const test::TempDir temp_dir("nebula-object-store-append");
    const storage::StorageRuntimeConfig route_config = make_route_config(temp_dir.path() / "files");
    const storage::ObjectStore store(route_config);
    test::expect_true(store.ensure_root_dirs(), "object store should create root dirs before append");

    const std::filesystem::path temp_path = route_config.temp_dir / "chunk.part";
    test::expect_true(storage::ObjectStore::create_empty_temp_file(temp_path),
                      "object store should create empty temp file");

    const auto first_append = store.append_temp_chunk(temp_path, "hello", 0);
    test::expect_true(first_append.status == storage::AppendTempChunkStatus::Appended, "first append should succeed");
    test::expect_equal(test::read_all(temp_path), std::string("hello"), "first append should write chunk bytes");

    test::write_binary_file(temp_path, "hello-corrupted");
    test::expect_true(storage::ObjectStore::restore_temp_size(temp_path, 5),
                      "restore should trim temp file back to committed size");
    test::expect_equal(test::read_all(temp_path), std::string("hello"), "restore should truncate temp file");
}

void test_object_store_hash_publish_and_read_file() {
    const test::TempDir temp_dir("nebula-object-store-publish");
    const storage::StorageRuntimeConfig route_config = make_route_config(temp_dir.path() / "files");
    const storage::ObjectStore store(route_config);
    test::expect_true(store.ensure_root_dirs(), "object store should create root dirs before publish");

    const std::filesystem::path temp_path = route_config.temp_dir / "upload.part";
    test::write_binary_file(temp_path, "hello");

    const std::optional<nebula::storage::HashAndSize> hash = storage::ObjectStore::hash_file(temp_path);
    if (!hash.has_value()) {
        test::fail("hash_file should hash existing file");
    }
    test::expect_equal(hash->sha256_hex.size(), std::size_t{64}, "hash_file should return 64 hex chars for sha256");
    test::expect_true(common::is_valid_lower_hex_token(hash->sha256_hex, std::size_t{64}),
                      "hash_file should return lowercase hex sha256");

    const std::filesystem::path object_path =
        route_config.root_dir /
        std::format("objects/{}/{}/{}", hash->sha256_hex.substr(0, 2), hash->sha256_hex.substr(2, 2), hash->sha256_hex);
    test::expect_true(storage::ObjectStore::publish_object(temp_path, object_path, hash->sha256_hex,
                                                           hash->size_bytes) == storage::PublishObjectStatus::Created,
                      "publish_object should create new object file");

    const auto body = store.read_file(object_path);
    test::expect_true(body.has_value(), "read_file should read published object");
    test::expect_equal(*body, std::string("hello"), "read_file should return published bytes");
}

void test_object_store_scans_and_deletes_orphans() {
    const test::TempDir temp_dir("nebula-object-store-scan");
    const storage::StorageRuntimeConfig route_config = make_route_config(temp_dir.path() / "files");
    const storage::ObjectStore store(route_config);
    test::expect_true(store.ensure_root_dirs(), "object store should create root dirs before scan");

    const std::filesystem::path orphan_object = route_config.objects_dir / "aa" / "bb" / "orphan";
    const std::filesystem::path orphan_temp = route_config.temp_dir / "old.part";
    test::write_binary_file(orphan_object, "orphan-object");
    test::write_binary_file(orphan_temp, "orphan-temp");

    const std::unordered_set<std::string> no_objects;
    const std::unordered_set<std::string> no_temps;
    const std::vector<std::filesystem::path> orphan_objects =
        store.scan_orphan_objects(no_objects, std::chrono::seconds{0});
    const std::vector<std::filesystem::path> orphan_temps = store.scan_orphan_temps(no_temps, std::chrono::seconds{0});
    test::expect_equal(orphan_objects.size(), std::size_t{1}, "scan_orphan_objects should find orphan object file");
    test::expect_equal(orphan_temps.size(), std::size_t{1}, "scan_orphan_temps should find orphan temp file");

    test::expect_true(store.delete_object_path(orphan_object), "delete_object_path should remove orphan object");
    test::expect_true(store.delete_temp_path(orphan_temp), "delete_temp_path should remove orphan temp");
    test::expect_true(!std::filesystem::exists(orphan_object), "orphan object should be deleted");
    test::expect_true(!std::filesystem::exists(orphan_temp), "orphan temp should be deleted");
}

int run_object_store_tests() {
    const std::vector<nebula::test::TestCase> tests = {
        {"object store ensures root dirs", test_object_store_ensures_root_dirs},
        {"object store appends and restores temp file", test_object_store_appends_and_restores_temp_file},
        {"object store hashes publishes and reads file", test_object_store_hash_publish_and_read_file},
        {"object store scans and deletes orphans", test_object_store_scans_and_deletes_orphans},
    };

    return nebula::test::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::test::run_main(run_object_store_tests);
}
