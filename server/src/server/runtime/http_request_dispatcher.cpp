#include "nebula/server/runtime/http_request_dispatcher.hpp"

#include <exception>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "nebula/auth/application/service.hpp"
#include "nebula/auth/http/authentication.hpp"
#include "nebula/auth/http/responses.hpp"
#include "nebula/common/log/logger.hpp"
#include "nebula/http/redaction/request_redaction.hpp"
#include "nebula/http/codec/response_writer.hpp"
#include "nebula/http/routing/router.hpp"

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

HttpRequestDispatcher::HttpRequestDispatcher(std::shared_ptr<http::Router> router,
                                             std::shared_ptr<auth::AuthService> auth_service,
                                             common::ThreadPool& thread_pool, ResponseSubmitter response_submitter)
    : router_(std::move(router)),
      auth_service_(std::move(auth_service)),
      thread_pool_(&thread_pool),
      response_submitter_(std::move(response_submitter)) {
    if (router_ == nullptr) {
        throw std::invalid_argument("router_missing");
    }
    if (!response_submitter_) {
        throw std::invalid_argument("response_submitter_missing");
    }
}

void HttpRequestDispatcher::dispatch(ReactorRequestTask task) {
    thread_pool_->submit([this, task = std::move(task)]() mutable {
        ReactorResponseTask response_task{
            .reactor_id = task.reactor_id,
            .fd = task.fd,
            .connection_token = task.connection_token,
            .response = {},
            .close_after_write = task.close_after_write,
            .suppress_body = task.suppress_body,
            .request_line = std::move(task.request_line),
            .request_bytes = task.request_bytes,
            .request_started_at = task.request_started_at,
        };

        try {
            response_task.response = dispatch_request(std::move(task.request));
        } catch (const std::exception& e) {
            common::Logger::instance()
                .error("request handler exception")
                .field("fd", task.fd)
                .field("reactor_id", task.reactor_id)
                .field("connection_token", task.connection_token)
                .field("error", e.what())
                .field("next_state", "closing");
            response_task.response = http::make_api_error_response(http::HttpStatus::InternalServerError);
            response_task.close_after_write = true;
        } catch (...) {
            common::Logger::instance()
                .error("request handler exception")
                .field("fd", task.fd)
                .field("reactor_id", task.reactor_id)
                .field("connection_token", task.connection_token)
                .field("error", "unknown")
                .field("next_state", "closing");
            response_task.response = http::make_api_error_response(http::HttpStatus::InternalServerError);
            response_task.close_after_write = true;
        }

        try {
            response_submitter_(std::move(response_task));
        } catch (const std::exception& submit_error) {
            common::Logger::instance()
                .error("submit response failed")
                .field("fd", task.fd)
                .field("reactor_id", task.reactor_id)
                .field("connection_token", task.connection_token)
                .field("error", submit_error.what());
        } catch (...) {
            common::Logger::instance()
                .error("submit response failed")
                .field("fd", task.fd)
                .field("reactor_id", task.reactor_id)
                .field("connection_token", task.connection_token)
                .field("error", "unknown");
        }
    });
}

http::HttpResponse HttpRequestDispatcher::dispatch_request(http::HttpRequest request) const {
    http::RouteResolveResult resolved = router_->resolve(std::move(request));
    switch (resolved.status) {
        case http::RouteStatus::Matched: {
            if (resolved.handler == nullptr) {
                common::Logger::instance()
                    .error("route handler missing")
                    .field("error", "route_handler_missing")
                    .field("decision", "return_internal_error");
                return http::make_api_error_response(http::HttpStatus::InternalServerError);
            }

            if (resolved.options.require_user) {
                const auto authenticated =
                    auth::authenticate_http_request(auth_service_, resolved.context.request.headers);
                if (!authenticated.has_value()) {
                    common::Logger::instance()
                        .warn("request authentication failed")
                        .field("method", http::to_string(resolved.context.request.method))
                        .field("path", http::redact_request_path(resolved.context.request.path))
                        .field("error", auth::to_string(authenticated.error()))
                        .field("decision", "reject_request");
                    return auth::to_http_response(authenticated.error());
                }

                resolved.context.user = authenticated->user;
            }

            return (*resolved.handler)(resolved.context);
        }
        case http::RouteStatus::MethodNotAllowed: {
            http::HttpResponse response = http::make_api_error_response(http::HttpStatus::MethodNotAllowed);
            const std::string allow = format_allow_header(resolved.allowed_methods);
            if (!allow.empty()) {
                response.headers["Allow"] = allow;
            }
            return response;
        }
        case http::RouteStatus::NotFound: {
            return http::make_api_error_response(http::HttpStatus::NotFound);
        }
    }
    std::unreachable();
}

}  // namespace nebula::server
