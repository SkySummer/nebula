#ifndef NEBULA_SERVER_HTTP_SUB_REACTOR_POOL_HPP
#define NEBULA_SERVER_HTTP_SUB_REACTOR_POOL_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "nebula/net/listener.hpp"
#include "nebula/server/http_reactor_tasks.hpp"
#include "nebula/server/http_sub_reactor_callbacks.hpp"
#include "nebula/server/server_config.hpp"

namespace nebula::server {

class HttpSubReactor;

class HttpSubReactorPool {
public:
    using RequestDispatchFn = SubReactorRequestDispatchFn;
    using LifecycleProviderFn = SubReactorLifecycleProviderFn;
    using ForceCloseProviderFn = SubReactorForceCloseProviderFn;
    using FatalErrorFn = SubReactorFatalErrorFn;

    HttpSubReactorPool(const ServerConfig& config, RequestDispatchFn dispatch_request,
                       LifecycleProviderFn lifecycle_provider, ForceCloseProviderFn force_close_provider,
                       FatalErrorFn fatal_error_callback);
    ~HttpSubReactorPool();

    HttpSubReactorPool(const HttpSubReactorPool&) = delete;
    HttpSubReactorPool& operator=(const HttpSubReactorPool&) = delete;
    HttpSubReactorPool(HttpSubReactorPool&&) = delete;
    HttpSubReactorPool& operator=(HttpSubReactorPool&&) = delete;

    bool start();
    void shutdown();
    void request_stop_all();
    void notify_all();
    bool dispatch_connection(const net::AcceptedSocket& accepted, std::uint64_t connection_token);
    bool enqueue_response(ReactorResponseTask response);
    [[nodiscard]] bool all_drained(std::size_t& connection_count, std::size_t& pending_count) const;
    [[nodiscard]] std::size_t tracked_connection_count() const;

private:
    const ServerConfig* config_ = nullptr;
    RequestDispatchFn dispatch_request_;
    LifecycleProviderFn lifecycle_provider_;
    ForceCloseProviderFn force_close_provider_;
    FatalErrorFn fatal_error_callback_;

    std::vector<std::unique_ptr<HttpSubReactor>> sub_reactors_;
    std::size_t started_sub_reactor_count_ = 0;
    std::size_t next_sub_reactor_index_ = 0;
};

}  // namespace nebula::server

#endif  // NEBULA_SERVER_HTTP_SUB_REACTOR_POOL_HPP
