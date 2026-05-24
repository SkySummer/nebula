#ifndef NEBULA_SERVER_RUNTIME_HTTP_REQUEST_DISPATCHER_HPP
#define NEBULA_SERVER_RUNTIME_HTTP_REQUEST_DISPATCHER_HPP

#include <functional>
#include <memory>

#include "nebula/auth/application/service.hpp"
#include "nebula/common/runtime/thread_pool.hpp"
#include "nebula/http/routing/router.hpp"
#include "nebula/server/reactor/tasks.hpp"

namespace nebula::server {

class HttpRequestDispatcher {
public:
    using ResponseSubmitter = std::function<void(ReactorResponseTask task)>;

    HttpRequestDispatcher(std::shared_ptr<http::Router> router, std::shared_ptr<auth::AuthService> auth_service,
                          common::ThreadPool& thread_pool, ResponseSubmitter response_submitter);

    void dispatch(ReactorRequestTask task);

private:
    [[nodiscard]] http::HttpResponse dispatch_request(http::HttpRequest request) const;

    std::shared_ptr<http::Router> router_;
    std::shared_ptr<auth::AuthService> auth_service_;
    common::ThreadPool* thread_pool_ = nullptr;
    ResponseSubmitter response_submitter_;
};

}  // namespace nebula::server

#endif  // NEBULA_SERVER_RUNTIME_HTTP_REQUEST_DISPATCHER_HPP
