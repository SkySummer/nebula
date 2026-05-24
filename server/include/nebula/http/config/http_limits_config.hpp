#ifndef NEBULA_HTTP_CONFIG_HTTP_LIMITS_CONFIG_HPP
#define NEBULA_HTTP_CONFIG_HTTP_LIMITS_CONFIG_HPP

#include <cstddef>

namespace nebula::http {

struct HttpLimitsConfig {
    std::size_t max_header_bytes = std::size_t{16} * 1024U;
    std::size_t max_request_target_bytes = std::size_t{8} * 1024U;
    std::size_t max_body_bytes = std::size_t{8} * 1024U * 1024U;
};

}  // namespace nebula::http

#endif  // NEBULA_HTTP_CONFIG_HTTP_LIMITS_CONFIG_HPP
