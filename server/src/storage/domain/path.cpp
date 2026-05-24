#include "nebula/storage/domain/path.hpp"

#include <algorithm>
#include <utility>

#include "nebula/common/codec/base64.hpp"

namespace nebula::storage {

namespace {

[[nodiscard]] bool contains_invalid_filename_bytes(std::string_view segment) {
    return std::ranges::any_of(segment, [](unsigned char ch) { return ch == '\0' || ch < 0x20U || ch == 0x7FU; });
}

}  // namespace

bool validate_canonical_path(std::string_view path) {
    if (path.empty() || path.front() != '/') {
        return false;
    }
    if (path == "/") {
        return true;
    }
    if (path.back() == '/') {
        return false;
    }

    for (std::size_t begin = 1U; begin <= path.size();) {
        const std::size_t slash_pos = path.find('/', begin);
        const std::size_t end = slash_pos == std::string_view::npos ? path.size() : slash_pos;
        if (end == begin) {
            return false;
        }
        const std::string_view segment = path.substr(begin, end - begin);
        if (segment == "." || segment == ".." || contains_invalid_filename_bytes(segment)) {
            return false;
        }
        if (slash_pos == std::string_view::npos) {
            break;
        }
        begin = slash_pos + 1U;
    }
    return true;
}

std::string_view to_string(PathDecodeError error) noexcept {
    switch (error) {
        case PathDecodeError::InvalidEncoding:
            return "invalid_encoding";
        case PathDecodeError::InvalidCanonicalPath:
            return "invalid_canonical_path";
    }
    std::unreachable();
}

std::expected<std::string, PathDecodeError> decode_and_validate_path(std::string_view path_b64) {
    auto path = common::base64url_decode_to_string(path_b64);
    if (!path.has_value()) {
        return std::unexpected(PathDecodeError::InvalidEncoding);
    }
    if (!validate_canonical_path(*path)) {
        return std::unexpected(PathDecodeError::InvalidCanonicalPath);
    }

    return *path;
}

bool delete_file_if_exists(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return !ec;
}

void try_cleanup_empty_parents(const std::filesystem::path& file_path, const std::filesystem::path& stop_dir) {
    std::error_code ec;
    std::filesystem::path current = file_path.parent_path();
    while (!current.empty() && current != stop_dir &&
           current.generic_string().size() >= stop_dir.generic_string().size()) {
        ec.clear();
        const bool removed = std::filesystem::remove(current, ec);
        if (ec || !removed) {
            return;
        }
        current = current.parent_path();
    }
}

}  // namespace nebula::storage
