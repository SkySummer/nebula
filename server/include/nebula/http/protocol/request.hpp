#ifndef NEBULA_HTTP_PROTOCOL_REQUEST_HPP
#define NEBULA_HTTP_PROTOCOL_REQUEST_HPP

#include <string>
#include <unordered_map>
#include <vector>

#include "nebula/http/protocol/headers.hpp"
#include "nebula/http/protocol/method.hpp"

namespace nebula::http {

using QueryParams = std::unordered_map<std::string, std::vector<std::string>>;

struct HttpRequest {
    HttpMethod method = HttpMethod::Unknown;
    std::string path;
    QueryParams query_params;
    std::string request_line;
    HeaderMap headers;
    std::string body;
    bool keep_alive = true;
};

}  // namespace nebula::http

#endif  // NEBULA_HTTP_PROTOCOL_REQUEST_HPP
