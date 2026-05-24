#include "nebula/storage/http/responses.hpp"

#include <utility>

#include "nebula/http/codec/response_writer.hpp"

namespace nebula::storage {

http::HttpResponse to_http_response(StorageError error) {
    switch (error) {
        case StorageError::InvalidPath:
            return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_path", "invalid storage path");
        case StorageError::InvalidChunkIndex:
            return http::make_api_error_response(http::HttpStatus::Conflict, "invalid_chunk_index",
                                                 "unexpected chunk index");
        case StorageError::InvalidLimit:
            return http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_request",
                                                 "invalid recent query params");
        case StorageError::RootDeleteNotAllowed:
            return http::make_api_error_response(http::HttpStatus::BadRequest, "root_delete_not_allowed",
                                                 "cannot delete root");
        case StorageError::ParentNotFound:
            return http::make_api_error_response(http::HttpStatus::NotFound, "parent_not_found",
                                                 "parent directory not found");
        case StorageError::ParentNotDirectory:
            return http::make_api_error_response(http::HttpStatus::Conflict, "parent_not_directory",
                                                 "parent path is not a directory");
        case StorageError::PathConflict:
            return http::make_api_error_response(http::HttpStatus::Conflict, "path_conflict",
                                                 "storage path conflicts with an existing file or directory");
        case StorageError::PathNotFound:
            return http::make_api_error_response(http::HttpStatus::NotFound, "path_not_found", "path not found");
        case StorageError::NotDirectory:
            return http::make_api_error_response(http::HttpStatus::Conflict, "not_directory",
                                                 "path is not a directory");
        case StorageError::NotFile:
            return http::make_api_error_response(http::HttpStatus::Conflict, "not_file", "path is not a file");
        case StorageError::DirectoryAlreadyExists:
            return http::make_api_error_response(http::HttpStatus::Conflict, "directory_already_exists",
                                                 "directory already exists");
        case StorageError::UploadNotFound:
            return http::make_api_error_response(http::HttpStatus::NotFound, "upload_not_found", "upload not found");
        case StorageError::UploadAlreadyComplete:
            return http::make_api_error_response(http::HttpStatus::Conflict, "upload_already_complete",
                                                 "upload already complete");
        case StorageError::UploadIncomplete:
            return http::make_api_error_response(http::HttpStatus::Conflict, "upload_incomplete",
                                                 "upload is incomplete");
        case StorageError::FileTooLarge:
            return http::make_api_error_response(http::HttpStatus::ContentTooLarge, "file_too_large", "file too large");
        case StorageError::StorageQuotaExceeded:
            return http::make_api_error_response(http::HttpStatus::ContentTooLarge, "storage_quota_exceeded",
                                                 "storage quota exceeded");
        case StorageError::NonEmptyDirectory:
            return http::make_api_error_response(http::HttpStatus::Conflict, "non_empty_directory",
                                                 "directory is not empty");
        case StorageError::DownloadTicketInvalid:
            return http::make_api_error_response(http::HttpStatus::Unauthorized, "download_ticket_invalid",
                                                 "invalid download ticket");
        case StorageError::DownloadTicketExpired:
            return http::make_api_error_response(http::HttpStatus::Unauthorized, "download_ticket_expired",
                                                 "download ticket expired");
        case StorageError::InternalError:
            return http::make_api_error_response(http::HttpStatus::InternalServerError);
    }
    std::unreachable();
}

}  // namespace nebula::storage
