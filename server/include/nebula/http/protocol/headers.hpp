#ifndef NEBULA_HTTP_PROTOCOL_HEADERS_HPP
#define NEBULA_HTTP_PROTOCOL_HEADERS_HPP

#include <string>
#include <unordered_map>

namespace nebula::http {

using HeaderMap = std::unordered_map<std::string, std::string>;

}  // namespace nebula::http

#endif  // NEBULA_HTTP_PROTOCOL_HEADERS_HPP
