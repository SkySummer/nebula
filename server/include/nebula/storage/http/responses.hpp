#ifndef NEBULA_STORAGE_HTTP_RESPONSES_HPP
#define NEBULA_STORAGE_HTTP_RESPONSES_HPP

#include "nebula/http/protocol/response.hpp"
#include "nebula/storage/domain/error.hpp"

namespace nebula::storage {

[[nodiscard]] http::HttpResponse to_http_response(StorageError error);

}  // namespace nebula::storage

#endif  // NEBULA_STORAGE_HTTP_RESPONSES_HPP
