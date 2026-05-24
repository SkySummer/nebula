#ifndef NEBULA_SERVER_REACTOR_SUB_REACTOR_POOL_HPP
#define NEBULA_SERVER_REACTOR_SUB_REACTOR_POOL_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "nebula/http/config/http_limits_config.hpp"
#include "nebula/net/listener.hpp"
#include "nebula/server/config/config.hpp"
#include "nebula/server/config/timeouts_config.hpp"
#include "nebula/server/reactor/sub_reactor.hpp"
#include "nebula/server/reactor/tasks.hpp"

namespace nebula::server {

class SubReactorPool {
public:
    using RequestDispatcher = SubReactor::RequestDispatcher;
    using LifecycleProvider = SubReactor::LifecycleProvider;
    using ForceCloseChecker = SubReactor::ForceCloseChecker;
    using FatalErrorHandler = SubReactor::FatalErrorHandler;

    SubReactorPool(const ServerConfig& server_config, const ServerTimeoutConfig& timeouts,
                   const http::HttpLimitsConfig& limits, RequestDispatcher request_dispatcher,
                   LifecycleProvider lifecycle_provider, ForceCloseChecker force_close_checker,
                   FatalErrorHandler fatal_error_handler);
    ~SubReactorPool() noexcept;

    SubReactorPool(const SubReactorPool&) = delete;
    SubReactorPool& operator=(const SubReactorPool&) = delete;
    SubReactorPool(SubReactorPool&&) = delete;
    SubReactorPool& operator=(SubReactorPool&&) = delete;

    bool start();
    void shutdown();
    void request_stop_all();
    void notify_all();
    bool dispatch_connection(const net::AcceptedSocket& accepted, std::uint64_t connection_token);
    bool enqueue_response(ReactorResponseTask response);
    [[nodiscard]] bool all_drained(std::size_t& connection_count, std::size_t& pending_count) const;
    [[nodiscard]] std::size_t tracked_connection_count() const;

private:
    const ServerConfig* server_config_ = nullptr;
    const ServerTimeoutConfig* timeouts_ = nullptr;
    const http::HttpLimitsConfig* limits_ = nullptr;
    RequestDispatcher request_dispatcher_;
    LifecycleProvider lifecycle_provider_;
    ForceCloseChecker force_close_checker_;
    FatalErrorHandler fatal_error_handler_;

    std::vector<std::unique_ptr<SubReactor>> sub_reactors_;
    std::size_t started_sub_reactor_count_ = 0;
    std::size_t next_sub_reactor_index_ = 0;
};

}  // namespace nebula::server

#endif  // NEBULA_SERVER_REACTOR_SUB_REACTOR_POOL_HPP
