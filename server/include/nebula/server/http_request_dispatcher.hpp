#ifndef NEBULA_SERVER_HTTP_REQUEST_DISPATCHER_HPP
#define NEBULA_SERVER_HTTP_REQUEST_DISPATCHER_HPP

#include <functional>
#include <memory>

#include "nebula/common/thread_pool.hpp"
#include "nebula/http/http_types.hpp"
#include "nebula/server/http_reactor_tasks.hpp"

namespace nebula::http {

class Router;

}  // namespace nebula::http

namespace nebula::server {

class HttpRequestDispatcher {
public:
    using SubmitResponseFn = std::function<void(ReactorResponseTask task)>;

    HttpRequestDispatcher(std::shared_ptr<http::Router> router, common::ThreadPool& thread_pool,
                          SubmitResponseFn submit_response);

    void dispatch(ReactorRequestTask task);

private:
    [[nodiscard]] http::HttpResponse dispatch_request(const http::HttpRequest& request) const;
    void submit_error_response(ReactorRequestTask task);

    std::shared_ptr<http::Router> router_;
    common::ThreadPool* thread_pool_ = nullptr;
    SubmitResponseFn submit_response_;
};

}  // namespace nebula::server

#endif  // NEBULA_SERVER_HTTP_REQUEST_DISPATCHER_HPP
