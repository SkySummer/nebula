#ifndef NEBULA_SERVER_HTTP_REACTOR_TASKS_HPP
#define NEBULA_SERVER_HTTP_REACTOR_TASKS_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include "nebula/http/http_types.hpp"

namespace nebula::server {

struct ReactorRequestTask {
    std::size_t reactor_id = 0;
    int fd = -1;
    std::uint64_t connection_token = 0;
    http::HttpRequest request;
    bool close_after_write = false;
    bool suppress_body = false;
    std::string request_line;
    std::size_t request_bytes = 0;
    std::chrono::steady_clock::time_point request_started_at;
};

struct ReactorResponseTask {
    std::size_t reactor_id = 0;
    int fd = -1;
    std::uint64_t connection_token = 0;
    http::HttpResponse response;
    bool close_after_write = false;
    bool suppress_body = false;
    std::string request_line;
    std::size_t request_bytes = 0;
    std::chrono::steady_clock::time_point request_started_at;
};

}  // namespace nebula::server

#endif  // NEBULA_SERVER_HTTP_REACTOR_TASKS_HPP
