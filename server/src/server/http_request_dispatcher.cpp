#include "nebula/server/http_request_dispatcher.hpp"

#include <exception>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "nebula/auth/auth_http.hpp"
#include "nebula/auth/auth_service.hpp"
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

http::AuthenticatedUser to_authenticated_user(const auth::AuthenticateResult& authenticated) {
    return {
        .user_id = authenticated.user.user_id,
        .username = authenticated.user.username,
        .created_at_s = authenticated.user.created_at_s,
    };
}

}  // namespace

HttpRequestDispatcher::HttpRequestDispatcher(std::shared_ptr<http::Router> router,
                                             std::shared_ptr<auth::AuthService> auth_service,
                                             common::ThreadPool& thread_pool, SubmitResponseFn submit_response)
    : router_(std::move(router)),
      auth_service_(std::move(auth_service)),
      thread_pool_(&thread_pool),
      submit_response_(std::move(submit_response)) {}

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
        common::Logger::instance()
            .error(common::LogDomain::Server, "router missing")
            .field("error", "router_missing")
            .field("decision", "return_internal_error");
        return http::make_api_error_response(http::HttpStatus::InternalServerError);
    }

    http::RouteResolveResult resolved = router_->resolve(std::move(request));
    switch (resolved.status) {
        case http::RouteStatus::Matched: {
            if (resolved.handler == nullptr) {
                common::Logger::instance()
                    .error(common::LogDomain::Server, "route handler missing")
                    .field("error", "route_handler_missing")
                    .field("decision", "return_internal_error");
                return http::make_api_error_response(http::HttpStatus::InternalServerError);
            }

            if (resolved.options.require_user) {
                const auto authenticated =
                    auth::authenticate_http_request(auth_service_, resolved.context.request.headers);
                if (authenticated.error != auth::AuthErrorCode::Ok) {
                    common::Logger::instance()
                        .warn(common::LogDomain::Server, "request authentication failed")
                        .field("method", http::to_string(resolved.context.request.method))
                        .field("path", resolved.context.request.path)
                        .field("error", auth::to_string(authenticated.error))
                        .field("decision", "reject_request");
                    return auth::make_auth_error_response(authenticated.error);
                }

                resolved.context.user = to_authenticated_user(authenticated);
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
    return http::make_api_error_response(http::HttpStatus::InternalServerError);
}

void HttpRequestDispatcher::submit_error_response(ReactorRequestTask task) {
    if (!submit_response_) {
        throw std::runtime_error("response submit callback missing");
    }

    submit_response_(ReactorResponseTask{
        .reactor_id = task.reactor_id,
        .fd = task.fd,
        .connection_token = task.connection_token,
        .response = http::make_api_error_response(http::HttpStatus::InternalServerError),
        .close_after_write = true,
        .suppress_body = task.suppress_body,
        .request_line = std::move(task.request_line),
        .request_bytes = task.request_bytes,
        .request_started_at = task.request_started_at,
    });
}

}  // namespace nebula::server
