#ifndef NEBULA_HTTP_PROTOCOL_METHOD_HPP
#define NEBULA_HTTP_PROTOCOL_METHOD_HPP

#include <cstdint>
#include <string_view>

namespace nebula::http {

enum class HttpMethod : std::uint8_t {
    Unknown,
    Get,
    Head,
    Post,
    Put,
    Delete,
    Connect,
    Options,
    Trace,
};

[[nodiscard]] std::string_view to_string(HttpMethod method) noexcept;

[[nodiscard]] HttpMethod parse_method(std::string_view text);

}  // namespace nebula::http

#endif  // NEBULA_HTTP_PROTOCOL_METHOD_HPP
