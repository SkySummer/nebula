#ifndef NEBULA_COMMON_PLATFORM_FILE_IO_HPP
#define NEBULA_COMMON_PLATFORM_FILE_IO_HPP

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace nebula::common {

enum class ReadFileError : std::uint8_t {
    OpenFailed,
    ReadFailed,
    TooLarge,
};

enum class WriteFileError : std::uint8_t {
    OpenFailed,
    WriteFailed,
    FlushFailed,
    TooLarge,
};

[[nodiscard]] std::string_view to_string(ReadFileError error) noexcept;

[[nodiscard]] std::string_view to_string(WriteFileError error) noexcept;

[[nodiscard]] std::expected<std::string, ReadFileError> read_file(const std::filesystem::path& path);

[[nodiscard]] std::expected<std::string, ReadFileError> read_file(const std::filesystem::path& path,
                                                                  std::uintmax_t max_bytes);

[[nodiscard]] std::expected<void, WriteFileError> write_file(const std::filesystem::path& path,
                                                             std::string_view content);

[[nodiscard]] std::expected<void, WriteFileError> write_binary_file(const std::filesystem::path& path,
                                                                    std::span<const std::byte> content);

}  // namespace nebula::common

#endif  // NEBULA_COMMON_PLATFORM_FILE_IO_HPP
