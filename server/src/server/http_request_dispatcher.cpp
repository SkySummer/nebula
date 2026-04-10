#include "nebula/server/http_request_dispatcher.hpp"

#include <exception>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "nebula/common/logger.hpp"
#include "nebula/http/http_response_writer.hpp"
#include "nebula/http/router.hpp"

namespace nebula::server {

namespace {

std::string format_allow_header(const std::vector<http::HttpMethod>& allowed_methods) {
    std::string allow;
    for (std::size_t idx = 0; idx < allowed_methods.size(); ++idx) {
        if (idx != 0U) {
            allow.append(", ");
        }
        allow.append(http::to_string(allowed_methods[idx]));
    }
    return allow;
}

}  // namespace

HttpRequestDispatcher::HttpRequestDispatcher(std::shared_ptr<http::Router> router, common::ThreadPool& thread_pool,
                                             SubmitResponseFn submit_response)
    : router_(std::move(router)), thread_pool_(&thread_pool), submit_response_(std::move(submit_response)) {}

void HttpRequestDispatcher::dispatch(ReactorRequestTask task) {
    if (thread_pool_ == nullptr) {
        throw std::runtime_error("thread pool missing");
    }

    thread_pool_->submit([this, task = std::move(task)]() mutable {
        try {
            http::HttpResponse response = dispatch_request(std::move(task.request));
            if (!submit_response_) {
                throw std::runtime_error("response submit callback missing");
            }
            submit_response_(ReactorResponseTask{
                .reactor_id = task.reactor_id,
                .fd = task.fd,
                .connection_token = task.connection_token,
                .response = std::move(response),
                .close_after_write = task.close_after_write,
                .suppress_body = task.suppress_body,
                .request_line = std::move(task.request_line),
                .request_bytes = task.request_bytes,
                .request_started_at = task.request_started_at,
            });
        } catch (const std::exception& e) {
            common::Logger::instance()
                .error(common::LogDomain::Server, "request handler exception")
                .field("fd", task.fd)
                .field("reactor_id", task.reactor_id)
                .field("connection_token", task.connection_token)
                .field("error", e.what())
                .field("next_state", "closing");
            try {
                submit_error_response(std::move(task));
            } catch (const std::exception& submit_error) {
                common::Logger::instance()
                    .error(common::LogDomain::Server, "submit error response failed")
                    .field("fd", task.fd)
                    .field("reactor_id", task.reactor_id)
                    .field("connection_token", task.connection_token)
                    .field("error", submit_error.what())
                    .field("next_state", "closing");
            } catch (...) {
                common::Logger::instance()
                    .error(common::LogDomain::Server, "submit error response failed")
                    .field("fd", task.fd)
                    .field("reactor_id", task.reactor_id)
                    .field("connection_token", task.connection_token)
                    .field("error", "unknown")
                    .field("next_state", "closing");
            }
        } catch (...) {
            common::Logger::instance()
                .error(common::LogDomain::Server, "request handler exception")
                .field("fd", task.fd)
                .field("reactor_id", task.reactor_id)
                .field("connection_token", task.connection_token)
                .field("error", "unknown")
                .field("next_state", "closing");
            try {
                submit_error_response(std::move(task));
            } catch (...) {
                common::Logger::instance()
                    .error(common::LogDomain::Server, "submit error response failed")
                    .field("fd", task.fd)
                    .field("reactor_id", task.reactor_id)
                    .field("connection_token", task.connection_token)
                    .field("error", "unknown")
                    .field("next_state", "closing");
            }
        }
    });
}

http::HttpResponse HttpRequestDispatcher::dispatch_request(http::HttpRequest request) const {
    if (router_ == nullptr) {
        return http::make_error_response(http::HttpStatus::NotFound);
    }

    const http::RouteDispatchResult routed = router_->dispatch(std::move(request));
    if (routed.status == http::RouteStatus::Matched) {
        return routed.response;
    }
    if (routed.status == http::RouteStatus::MethodNotAllowed) {
        http::HttpResponse response = http::make_error_response(http::HttpStatus::MethodNotAllowed);
        const std::string allow = format_allow_header(routed.allowed_methods);
        if (!allow.empty()) {
            response.headers["Allow"] = allow;
        }
        return response;
    }
    return http::make_error_response(http::HttpStatus::NotFound);
}

void HttpRequestDispatcher::submit_error_response(ReactorRequestTask task) {
    if (!submit_response_) {
        throw std::runtime_error("response submit callback missing");
    }

    submit_response_(ReactorResponseTask{
        .reactor_id = task.reactor_id,
        .fd = task.fd,
        .connection_token = task.connection_token,
        .response = http::make_error_response(http::HttpStatus::InternalServerError, {}),
        .close_after_write = true,
        .suppress_body = task.suppress_body,
        .request_line = std::move(task.request_line),
        .request_bytes = task.request_bytes,
        .request_started_at = task.request_started_at,
    });
}

}  // namespace nebula::server
