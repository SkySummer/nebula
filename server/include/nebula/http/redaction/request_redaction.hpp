#ifndef NEBULA_HTTP_REDACTION_REQUEST_REDACTION_HPP
#define NEBULA_HTTP_REDACTION_REQUEST_REDACTION_HPP

#include <string>
#include <string_view>

namespace nebula::http {

[[nodiscard]] std::string redact_request_path(std::string_view path);

[[nodiscard]] std::string redact_request_line(std::string_view request_line);

}  // namespace nebula::http

#endif  // NEBULA_HTTP_REDACTION_REQUEST_REDACTION_HPP
