#include "nebula/common/platform/file_io.hpp"

#include <algorithm>
#include <fstream>
#include <ios>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace nebula::common {

namespace {

[[nodiscard]] std::optional<std::uintmax_t> query_stream_size(std::ifstream& stream) {
    stream.seekg(0, std::ios::end);
    if (stream.fail()) {
        return std::nullopt;
    }

    const std::ifstream::pos_type end_pos = stream.tellg();
    if (end_pos == std::ifstream::pos_type(-1)) {
        return std::nullopt;
    }

    stream.seekg(0, std::ios::beg);
    if (stream.fail()) {
        return std::nullopt;
    }

    return static_cast<std::uintmax_t>(end_pos);
}

[[nodiscard]] std::expected<std::string, ReadFileError> read_file_impl(const std::filesystem::path& path,
                                                                       const std::optional<std::uintmax_t> max_bytes) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return std::unexpected(ReadFileError::OpenFailed);
    }

    auto size_result = query_stream_size(stream);
    if (!size_result.has_value()) {
        return std::unexpected(ReadFileError::ReadFailed);
    }

    const std::uintmax_t file_size = *size_result;
    if (max_bytes.has_value() && file_size > *max_bytes) {
        return std::unexpected(ReadFileError::TooLarge);
    }
    if (file_size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()) ||
        file_size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        return std::unexpected(ReadFileError::TooLarge);
    }

    std::string content(static_cast<std::size_t>(file_size), '\0');
    if (!content.empty()) {
        stream.read(content.data(), static_cast<std::streamsize>(content.size()));
        if (stream.gcount() != static_cast<std::streamsize>(content.size()) || !stream.good()) {
            return std::unexpected(ReadFileError::ReadFailed);
        }
    }

    return content;
}

[[nodiscard]] std::expected<void, WriteFileError> write_text_file_impl(std::ofstream& stream,
                                                                       std::string_view content) {
    if (!stream.is_open()) {
        return std::unexpected(WriteFileError::OpenFailed);
    }

    if (!content.empty()) {
        if (!std::in_range<std::streamsize>(content.size())) {
            return std::unexpected(WriteFileError::TooLarge);
        }

        stream.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!stream.good()) {
            return std::unexpected(WriteFileError::WriteFailed);
        }
    }

    stream.flush();
    if (!stream.good()) {
        return std::unexpected(WriteFileError::FlushFailed);
    }

    return {};
}

[[nodiscard]] std::expected<void, WriteFileError> write_binary_file_impl(std::ofstream& stream,
                                                                         std::span<const std::byte> content) {
    if (!stream.is_open()) {
        return std::unexpected(WriteFileError::OpenFailed);
    }

    if (!content.empty()) {
        if (!std::in_range<std::streamsize>(content.size())) {
            return std::unexpected(WriteFileError::TooLarge);
        }

        std::vector<char> buffer(content.size(), '\0');
        std::ranges::transform(content, buffer.begin(), [](const std::byte value) { return static_cast<char>(value); });
        stream.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        if (!stream.good()) {
            return std::unexpected(WriteFileError::WriteFailed);
        }
    }

    stream.flush();
    if (!stream.good()) {
        return std::unexpected(WriteFileError::FlushFailed);
    }

    return {};
}

}  // namespace

std::string_view to_string(ReadFileError error) noexcept {
    switch (error) {
        case ReadFileError::OpenFailed:
            return "open_failed";
        case ReadFileError::ReadFailed:
            return "read_failed";
        case ReadFileError::TooLarge:
            return "too_large";
    }
    std::unreachable();
}

std::string_view to_string(WriteFileError error) noexcept {
    switch (error) {
        case WriteFileError::OpenFailed:
            return "open_failed";
        case WriteFileError::WriteFailed:
            return "write_failed";
        case WriteFileError::FlushFailed:
            return "flush_failed";
        case WriteFileError::TooLarge:
            return "too_large";
    }
    std::unreachable();
}

std::expected<std::string, ReadFileError> read_file(const std::filesystem::path& path) {
    return read_file_impl(path, std::nullopt);
}

std::expected<std::string, ReadFileError> read_file(const std::filesystem::path& path, const std::uintmax_t max_bytes) {
    return read_file_impl(path, max_bytes);
}

std::expected<void, WriteFileError> write_file(const std::filesystem::path& path, std::string_view content) {
    std::ofstream stream(path, std::ios::trunc);
    return write_text_file_impl(stream, content);
}

std::expected<void, WriteFileError> write_binary_file(const std::filesystem::path& path,
                                                      std::span<const std::byte> content) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    return write_binary_file_impl(stream, content);
}

}  // namespace nebula::common
