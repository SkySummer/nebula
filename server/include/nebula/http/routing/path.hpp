#ifndef NEBULA_HTTP_ROUTING_PATH_HPP
#define NEBULA_HTTP_ROUTING_PATH_HPP

#include <string>
#include <string_view>
#include <vector>

namespace nebula::http {

[[nodiscard]] std::vector<std::string> split_http_path_segments(std::string_view path);

}  // namespace nebula::http

#endif  // NEBULA_HTTP_ROUTING_PATH_HPP
