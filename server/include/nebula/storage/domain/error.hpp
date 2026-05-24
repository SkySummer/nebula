#ifndef NEBULA_STORAGE_DOMAIN_ERROR_HPP
#define NEBULA_STORAGE_DOMAIN_ERROR_HPP

#include <cstdint>
#include <string_view>

namespace nebula::storage {

enum class StorageError : std::uint8_t {
    InvalidPath,
    InvalidChunkIndex,
    InvalidLimit,
    RootDeleteNotAllowed,
    ParentNotFound,
    ParentNotDirectory,
    PathConflict,
    PathNotFound,
    NotDirectory,
    NotFile,
    DirectoryAlreadyExists,
    UploadNotFound,
    UploadAlreadyComplete,
    UploadIncomplete,
    FileTooLarge,
    StorageQuotaExceeded,
    NonEmptyDirectory,
    DownloadTicketInvalid,
    DownloadTicketExpired,
    InternalError,
};

[[nodiscard]] std::string_view to_string(StorageError error) noexcept;

}  // namespace nebula::storage

#endif  // NEBULA_STORAGE_DOMAIN_ERROR_HPP
