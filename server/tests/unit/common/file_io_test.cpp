#include "nebula/common/platform/file_io.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "nebula_tests/common.hpp"

namespace {

using namespace nebula;

void test_read_file_reads_text_content() {
    const test::TempDir dir("nebula-file-io-read");
    const std::filesystem::path file = dir.path() / "config.toml";

    const auto write_result = nebula::common::write_file(file, "hello");
    test::expect_true(write_result.has_value(), "write_file should create text file");

    const auto read_result = nebula::common::read_file(file);
    test::expect_true(read_result.has_value(), "read_file should read text file");
    test::expect_equal(*read_result, std::string("hello"), "read_file should return persisted content");
}

void test_read_file_reports_open_failed_for_missing_file() {
    const test::TempDir dir("nebula-file-io-missing");
    const std::filesystem::path file = dir.path() / "missing.txt";

    const auto read_result = nebula::common::read_file(file);
    test::expect_true(!read_result.has_value(), "read_file should fail for missing file");
    test::expect_equal(read_result.error(), common::ReadFileError::OpenFailed,
                       "missing file should map to open_failed");
}

void test_read_file_reports_too_large_when_limit_exceeded() {
    const test::TempDir dir("nebula-file-io-too-large");
    const std::filesystem::path file = dir.path() / "payload.bin";
    const std::vector<std::byte> bytes = {std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};

    const auto write_result = nebula::common::write_binary_file(file, bytes);
    test::expect_true(write_result.has_value(), "write_binary_file should create binary file");

    const auto read_result = nebula::common::read_file(file, 2U);
    test::expect_true(!read_result.has_value(), "read_file should reject file that exceeds max size");
    test::expect_equal(read_result.error(), common::ReadFileError::TooLarge, "size overflow should map to too_large");
}

void test_write_file_reports_open_failed_for_missing_parent_dir() {
    const test::TempDir dir("nebula-file-io-write-open-failed");
    const std::filesystem::path file = dir.path() / "missing" / "config.toml";

    const auto write_result = nebula::common::write_file(file, "hello");
    test::expect_true(!write_result.has_value(), "write_file should fail when parent directory is missing");
    test::expect_equal(write_result.error(), common::WriteFileError::OpenFailed,
                       "missing parent directory should map to open_failed");
}

void test_write_binary_file_persists_bytes() {
    const test::TempDir dir("nebula-file-io-write");
    const std::filesystem::path file = dir.path() / "payload.bin";
    const std::vector<std::byte> bytes = {std::byte{0x00}, std::byte{0x7f}, std::byte{0xff}};

    const auto write_result = nebula::common::write_binary_file(file, bytes);
    test::expect_true(write_result.has_value(), "write_binary_file should persist bytes");

    const auto read_result = nebula::common::read_file(file);
    test::expect_true(read_result.has_value(), "read_file should read binary file");
    test::expect_equal(read_result->size(), std::size_t{3}, "binary file should preserve size");
    test::expect_equal(static_cast<unsigned char>((*read_result)[0]), static_cast<unsigned char>(0x00),
                       "binary file should preserve first byte");
    test::expect_equal(static_cast<unsigned char>((*read_result)[1]), static_cast<unsigned char>(0x7f),
                       "binary file should preserve second byte");
    test::expect_equal(static_cast<unsigned char>((*read_result)[2]), static_cast<unsigned char>(0xff),
                       "binary file should preserve third byte");
}

int run_file_io_tests() {
    const std::vector<nebula::test::TestCase> tests = {
        {"read file reads text content", test_read_file_reads_text_content},
        {"read file reports open failed for missing file", test_read_file_reports_open_failed_for_missing_file},
        {"read file reports too large when limit exceeded", test_read_file_reports_too_large_when_limit_exceeded},
        {"write file reports open failed for missing parent dir",
         test_write_file_reports_open_failed_for_missing_parent_dir},
        {"write binary file persists bytes", test_write_binary_file_persists_bytes},
    };

    return nebula::test::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::test::run_main(run_file_io_tests);
}
