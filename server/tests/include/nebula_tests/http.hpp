#ifndef NEBULA_TESTS_HTTP_HPP
#define NEBULA_TESTS_HTTP_HPP

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include <unistd.h>

#include "nebula/common/codec/json.hpp"
#include "nebula_tests/common.hpp"
#include "nebula_tests/integration.hpp"

namespace nebula::test::http {

inline std::string send_single_request(std::uint16_t port, std::string_view request) {
    const int fd = integration::connect_localhost(port);
    expect_true(fd >= 0, "connect should succeed");
    expect_true(integration::send_all(fd, request), "send request should succeed");
    const std::string response = integration::read_until_close(fd);
    ::close(fd);
    return response;
}

inline std::string response_body(std::string_view response) {
    const std::size_t header_end = response.find("\r\n\r\n");
    if (header_end == std::string_view::npos) {
        return {};
    }
    return std::string(response.substr(header_end + 4U));
}

inline std::string build_http_request(std::string_view method, std::string_view path, std::string_view body = {},
                                      std::string_view content_type = "application/json",
                                      std::string_view access_token = {}) {
    const bool include_content_headers = !body.empty() || method == "POST" || method == "PUT" || method == "DELETE";
    std::string headers = "Host: localhost\r\nConnection: close\r\n";
    if (!access_token.empty()) {
        headers.append(std::format("Authorization: Bearer {}\r\n", access_token));
    }

    if (include_content_headers) {
        if (!content_type.empty()) {
            headers.append(std::format("Content-Type: {}\r\n", content_type));
        }
        headers.append(std::format("Content-Length: {}\r\n", body.size()));
    }

    return std::format("{} {} HTTP/1.1\r\n{}\r\n{}", method, path, headers, body);
}

inline std::optional<nebula::common::JsonObject> api_data_object(std::string_view json) {
    const nebula::common::JsonParseResult parsed = nebula::common::parse_json(json);
    if (!parsed.ok) {
        return std::nullopt;
    }

    const nebula::common::JsonObject* object = parsed.value.get_if_object();
    if (object == nullptr) {
        return std::nullopt;
    }

    const auto data_it = object->find("data");
    if (data_it == object->end()) {
        return std::nullopt;
    }

    const nebula::common::JsonObject* data = data_it->second.get_if_object();
    if (data == nullptr) {
        return std::nullopt;
    }
    return *data;
}

inline std::optional<std::string> extract_api_data_string_field(std::string_view json, std::string_view key) {
    const std::optional<nebula::common::JsonObject> data = api_data_object(json);
    if (!data.has_value()) {
        return std::nullopt;
    }

    const auto value_it = data->find(std::string(key));
    if (value_it == data->end()) {
        return std::nullopt;
    }

    const std::string* value = value_it->second.get_if_string();
    if (value == nullptr) {
        return std::nullopt;
    }
    return *value;
}

inline std::optional<std::int64_t> extract_api_data_int64_field(std::string_view json, std::string_view key) {
    const std::optional<nebula::common::JsonObject> data = api_data_object(json);
    if (!data.has_value()) {
        return std::nullopt;
    }

    const auto value_it = data->find(std::string(key));
    if (value_it == data->end()) {
        return std::nullopt;
    }

    const std::int64_t* value = value_it->second.get_if_int64();
    if (value == nullptr) {
        return std::nullopt;
    }
    return *value;
}

}  // namespace nebula::test::http

#endif  // NEBULA_TESTS_HTTP_HPP
