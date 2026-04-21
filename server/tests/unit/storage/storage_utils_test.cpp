#include "nebula/storage/storage_utils.hpp"

#include <filesystem>
#include <vector>

#include "nebula_tests/test_support.hpp"

namespace {

using nebula::storage::delete_file_if_exists;
using nebula::testsupport::expect_true;
using nebula::testsupport::TempDir;
using nebula::testsupport::write_file;

void test_delete_file_if_exists_removes_existing_file() {
    const TempDir temp_dir("nebula-storage-utils-remove-existing-file");
    const std::filesystem::path file_path = temp_dir.path() / "existing.txt";
    write_file(file_path, "payload");

    std::error_code exists_error;
    expect_true(std::filesystem::exists(file_path, exists_error), "test file should exist before delete");
    expect_true(!exists_error, "test file exists check should not fail before delete");

    expect_true(delete_file_if_exists(file_path), "existing file should be removed successfully");

    exists_error.clear();
    expect_true(!std::filesystem::exists(file_path, exists_error), "test file should not exist after delete");
    expect_true(!exists_error, "test file exists check should not fail after delete");
}

void test_delete_file_if_exists_returns_true_when_file_missing() {
    const TempDir temp_dir("nebula-storage-utils-missing-file");
    const std::filesystem::path missing_file = temp_dir.path() / "missing.txt";

    expect_true(delete_file_if_exists(missing_file), "missing file cleanup should be treated as success");
}

void test_delete_file_if_exists_returns_false_for_non_empty_directory() {
    const TempDir temp_dir("nebula-storage-utils-non-empty-dir");
    const std::filesystem::path dir_path = temp_dir.path() / "non-empty";
    const std::filesystem::path child_file_path = dir_path / "child.txt";
    std::error_code ec;
    std::filesystem::create_directories(dir_path, ec);
    expect_true(!ec, "non-empty directory should be created");
    write_file(child_file_path, "payload");

    expect_true(!delete_file_if_exists(dir_path), "non-empty directory cleanup should report failure");

    ec.clear();
    expect_true(std::filesystem::exists(dir_path, ec), "non-empty directory should still exist after failed delete");
    expect_true(!ec, "non-empty directory exists check should not fail");
}

int run_storage_utils_tests() {
    const std::vector<nebula::testsupport::TestCase> tests = {
        {"delete file if exists removes existing file", test_delete_file_if_exists_removes_existing_file},
        {"delete file if exists returns true when file missing",
         test_delete_file_if_exists_returns_true_when_file_missing},
        {"delete file if exists returns false for non-empty directory",
         test_delete_file_if_exists_returns_false_for_non_empty_directory},
    };

    return nebula::testsupport::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::testsupport::run_main(run_storage_utils_tests);
}
