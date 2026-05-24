#include "nebula/storage/domain/path.hpp"

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "nebula/common/codec/base64.hpp"
#include "nebula_tests/common.hpp"

namespace {

using namespace nebula;

void test_delete_file_if_exists_removes_existing_file() {
    const test::TempDir temp_dir("nebula-storage-utils-remove-existing-file");
    const std::filesystem::path file_path = temp_dir.path() / "existing.txt";
    test::write_file(file_path, "payload");

    std::error_code exists_error;
    test::expect_true(std::filesystem::exists(file_path, exists_error), "test file should exist before delete");
    test::expect_true(!exists_error, "test file exists check should not fail before delete");

    test::expect_true(storage::delete_file_if_exists(file_path), "existing file should be removed successfully");

    exists_error.clear();
    test::expect_true(!std::filesystem::exists(file_path, exists_error), "test file should not exist after delete");
    test::expect_true(!exists_error, "test file exists check should not fail after delete");
}

void test_delete_file_if_exists_returns_true_when_file_missing() {
    const test::TempDir temp_dir("nebula-storage-utils-missing-file");
    const std::filesystem::path missing_file = temp_dir.path() / "missing.txt";

    test::expect_true(storage::delete_file_if_exists(missing_file),
                      "missing file cleanup should be treated as success");
}

void test_delete_file_if_exists_returns_false_for_non_empty_directory() {
    const test::TempDir temp_dir("nebula-storage-utils-non-empty-dir");
    const std::filesystem::path dir_path = temp_dir.path() / "non-empty";
    const std::filesystem::path child_file_path = dir_path / "child.txt";
    std::error_code ec;
    std::filesystem::create_directories(dir_path, ec);
    test::expect_true(!ec, "non-empty directory should be created");
    test::write_file(child_file_path, "payload");

    test::expect_true(!storage::delete_file_if_exists(dir_path), "non-empty directory cleanup should report failure");

    ec.clear();
    test::expect_true(std::filesystem::exists(dir_path, ec),
                      "non-empty directory should still exist after failed delete");
    test::expect_true(!ec, "non-empty directory exists check should not fail");
}

void test_validate_canonical_path_accepts_valid_paths() {
    test::expect_true(storage::validate_canonical_path("/"), "root path should be valid");
    test::expect_true(storage::validate_canonical_path("/docs"), "single segment path should be valid");
    test::expect_true(storage::validate_canonical_path("/docs/2026/report.txt"), "multi-segment path should be valid");
    test::expect_true(storage::validate_canonical_path("/docs/hello world.txt"), "space in segment should be valid");
    test::expect_true(storage::validate_canonical_path("/docs/中文-report.txt"), "utf-8 segment should be valid");
}

void test_validate_canonical_path_rejects_invalid_paths() {
    test::expect_true(!storage::validate_canonical_path(""), "empty path should be invalid");
    test::expect_true(!storage::validate_canonical_path("docs"), "relative path should be invalid");
    test::expect_true(!storage::validate_canonical_path("/docs/"), "trailing slash path should be invalid");
    test::expect_true(!storage::validate_canonical_path("/docs//report.txt"), "double slash path should be invalid");
    test::expect_true(!storage::validate_canonical_path("/./report.txt"), "dot segment path should be invalid");
    test::expect_true(!storage::validate_canonical_path("/docs/../report.txt"),
                      "dot-dot segment path should be invalid");
    test::expect_true(!storage::validate_canonical_path("/docs/\nreport.txt"), "newline in segment should be invalid");
    test::expect_true(!storage::validate_canonical_path("/docs/\treport.txt"), "tab in segment should be invalid");
    test::expect_true(!storage::validate_canonical_path(std::string_view("/docs/\x7Freport.txt", 17)),
                      "delete control byte in segment should be invalid");
    test::expect_true(!storage::validate_canonical_path(std::string_view("/docs/\0report.txt", 17)),
                      "nul byte in segment should be invalid");
}

void test_decode_and_validate_path_accepts_valid_base64_path() {
    const std::string encoded_path = nebula::common::base64url_encode("/docs/report.txt");
    const auto decoded_path = storage::decode_and_validate_path(encoded_path);
    if (!decoded_path.has_value()) {
        test::fail("valid base64 path should decode successfully");
    }
    test::expect_equal(*decoded_path, std::string("/docs/report.txt"), "decoded path should match input path");
}

void test_decode_and_validate_path_rejects_invalid_path() {
    const std::string encoded_path = nebula::common::base64url_encode("/docs/../report.txt");
    const auto decoded_path = storage::decode_and_validate_path(encoded_path);
    test::expect_true(!decoded_path.has_value(), "invalid canonical path should be rejected");
    test::expect_equal(decoded_path.error(), storage::PathDecodeError::InvalidCanonicalPath,
                       "invalid canonical path should return stable error");
}

void test_decode_and_validate_path_rejects_invalid_base64() {
    const auto decoded_path = storage::decode_and_validate_path("%%%");
    test::expect_true(!decoded_path.has_value(), "invalid base64 should be rejected");
    test::expect_equal(decoded_path.error(), storage::PathDecodeError::InvalidEncoding,
                       "invalid base64 should return stable error");
}

int run_path_tests() {
    const std::vector<nebula::test::TestCase> tests = {
        {"delete file if exists removes existing file", test_delete_file_if_exists_removes_existing_file},
        {"delete file if exists returns true when file missing",
         test_delete_file_if_exists_returns_true_when_file_missing},
        {"delete file if exists returns false for non-empty directory",
         test_delete_file_if_exists_returns_false_for_non_empty_directory},
        {"validate canonical path accepts valid paths", test_validate_canonical_path_accepts_valid_paths},
        {"validate canonical path rejects invalid paths", test_validate_canonical_path_rejects_invalid_paths},
        {"decode and validate path accepts valid base64 path", test_decode_and_validate_path_accepts_valid_base64_path},
        {"decode and validate path rejects invalid path", test_decode_and_validate_path_rejects_invalid_path},
        {"decode and validate path rejects invalid base64", test_decode_and_validate_path_rejects_invalid_base64},
    };

    return nebula::test::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::test::run_main(run_path_tests);
}
