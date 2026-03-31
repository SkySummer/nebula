#include "nebula/server/http_sub_reactor_pool.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <utility>

#include "nebula/common/logger.hpp"
#include "nebula/common/posix_utils.hpp"
#include "nebula/server/http_sub_reactor.hpp"

namespace nebula::server {

HttpSubReactorPool::HttpSubReactorPool(const ServerConfig& config, RequestDispatchFn dispatch_request,
                                       LifecycleProviderFn lifecycle_provider,
                                       ForceCloseProviderFn force_close_provider, FatalErrorFn fatal_error_callback)
    : config_(&config),
      dispatch_request_(std::move(dispatch_request)),
      lifecycle_provider_(std::move(lifecycle_provider)),
      force_close_provider_(std::move(force_close_provider)),
      fatal_error_callback_(std::move(fatal_error_callback)) {}

HttpSubReactorPool::~HttpSubReactorPool() = default;

bool HttpSubReactorPool::start() {
    next_sub_reactor_index_ = 0;

    if (config_ == nullptr) {
        common::Logger::instance().error("init sub reactors failed").field("error", "config_missing");
        return false;
    }

    if (sub_reactors_.empty()) {
        sub_reactors_.reserve(config_->sub_reactor_count);
        for (std::size_t idx = 0; idx < config_->sub_reactor_count; ++idx) {
            auto reactor = std::make_unique<HttpSubReactor>(idx, *config_, dispatch_request_, lifecycle_provider_,
                                                            force_close_provider_, fatal_error_callback_);
            sub_reactors_.push_back(std::move(reactor));
        }
    }

    if (sub_reactors_.size() != config_->sub_reactor_count) {
        common::Logger::instance()
            .error("init sub reactors failed")
            .field("count", sub_reactors_.size())
            .field("expected_count", config_->sub_reactor_count)
            .field("error", "sub_reactor_count_mismatch");
        return false;
    }

    std::size_t started_count = 0;
    for (auto& reactor : sub_reactors_) {
        if (!reactor->start()) {
            started_sub_reactor_count_ = started_count;
            shutdown();
            return false;
        }
        ++started_count;
    }

    started_sub_reactor_count_ = started_count;
    common::Logger::instance().info("sub reactors started").field("count", started_sub_reactor_count_);
    return true;
}

void HttpSubReactorPool::shutdown() {
    std::size_t dropped_pending_count = 0;
    const std::size_t started_count = started_sub_reactor_count_;

    request_stop_all();
    notify_all();

    for (auto& reactor : sub_reactors_) {
        dropped_pending_count += reactor->shutdown();
    }

    if (started_count > 0) {
        common::Logger::instance().info("sub reactors stopped").field("count", started_count);
    }

    started_sub_reactor_count_ = 0;

    if (dropped_pending_count > 0) {
        common::Logger::instance().info("pending responses cleared").field("count", dropped_pending_count);
    }
}

void HttpSubReactorPool::request_stop_all() {
    for (auto& reactor : sub_reactors_) {
        reactor->request_stop();
    }
}

void HttpSubReactorPool::notify_all() {
    for (auto& reactor : sub_reactors_) {
        reactor->notify_wakeup();
    }
}

bool HttpSubReactorPool::dispatch_connection(const net::AcceptedSocket& accepted, std::uint64_t connection_token) {
    if (sub_reactors_.empty()) {
        common::Logger::instance()
            .error("dispatch connection failed")
            .field("fd", accepted.fd)
            .field("peer", accepted.peer)
            .field("error", "no_sub_reactor")
            .field("next_state", "closing");
        common::close_fd(accepted.fd);
        return false;
    }

    const std::size_t reactor_id = next_sub_reactor_index_ % sub_reactors_.size();
    next_sub_reactor_index_ = (next_sub_reactor_index_ + 1U) % sub_reactors_.size();

    HttpSubReactor::AcceptedConnection pending;
    pending.fd = accepted.fd;
    pending.token = connection_token;
    pending.peer = accepted.peer;

    try {
        if (!sub_reactors_[reactor_id]->enqueue_accept(std::move(pending))) {
            common::Logger::instance()
                .debug("dispatch connection failed")
                .field("fd", accepted.fd)
                .field("peer", accepted.peer)
                .field("reactor_id", reactor_id)
                .field("error", "sub_reactor_not_running")
                .field("next_state", "closing");
            common::close_fd(accepted.fd);
            return false;
        }
    } catch (const std::exception& e) {
        common::Logger::instance()
            .error("dispatch connection failed")
            .field("fd", accepted.fd)
            .field("peer", accepted.peer)
            .field("reactor_id", reactor_id)
            .field("error", e.what())
            .field("next_state", "closing");
        common::close_fd(accepted.fd);
        return false;
    } catch (...) {
        common::Logger::instance()
            .error("dispatch connection failed")
            .field("fd", accepted.fd)
            .field("peer", accepted.peer)
            .field("reactor_id", reactor_id)
            .field("error", "unknown")
            .field("next_state", "closing");
        common::close_fd(accepted.fd);
        return false;
    }

    common::Logger::instance()
        .debug("connection dispatched")
        .field("fd", accepted.fd)
        .field("peer", accepted.peer)
        .field("reactor_id", reactor_id);
    return true;
}

bool HttpSubReactorPool::enqueue_response(ReactorResponseTask response) {
    const std::size_t reactor_id = response.reactor_id;
    const int fd = response.fd;
    const std::uint64_t connection_token = response.connection_token;
    if (reactor_id >= sub_reactors_.size()) {
        common::Logger::instance()
            .debug("response dropped")
            .field("fd", fd)
            .field("reactor_id", reactor_id)
            .field("connection_token", connection_token)
            .field("error", "reactor_not_found");
        return false;
    }

    const bool enqueued = sub_reactors_[reactor_id]->enqueue_response(std::move(response));

    if (!enqueued) {
        common::Logger::instance()
            .debug("response dropped")
            .field("fd", fd)
            .field("reactor_id", reactor_id)
            .field("connection_token", connection_token)
            .field("error", "sub_reactor_not_running");
        return false;
    }
    return true;
}

bool HttpSubReactorPool::all_drained(std::size_t& connection_count, std::size_t& pending_count) const {
    connection_count = 0;
    pending_count = 0;
    bool all_drained = true;

    for (const auto& reactor : sub_reactors_) {
        if (reactor->is_running()) {
            all_drained = false;
        }
        connection_count += reactor->connection_count();
        pending_count += reactor->pending_accept_count();
        pending_count += reactor->pending_response_count();
    }

    if (connection_count > 0 || pending_count > 0) {
        all_drained = false;
    }
    return all_drained;
}

std::size_t HttpSubReactorPool::tracked_connection_count() const {
    std::size_t count = 0;
    for (const auto& reactor : sub_reactors_) {
        count += reactor->tracked_connection_count();
    }
    return count;
}

}  // namespace nebula::server
