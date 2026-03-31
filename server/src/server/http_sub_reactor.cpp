#include "nebula/server/http_sub_reactor.hpp"

#include <cerrno>
#include <cstdio>
#include <exception>
#include <string>
#include <utility>
#include <vector>

#include <sys/eventfd.h>

#include "nebula/common/logger.hpp"
#include "nebula/common/posix_utils.hpp"
#include "nebula/net/eventfd_utils.hpp"
#include "nebula/server/reactor_constants.hpp"

namespace nebula::server {

namespace {

void report_destructor_cleanup_error(const char* action, const char* error) noexcept {
    std::fputs("sub reactor destructor cleanup failed: action=", stderr);
    std::fputs(action != nullptr ? action : "unknown", stderr);
    std::fputs(", error=", stderr);
    std::fputs(error != nullptr ? error : "unknown", stderr);
    std::fputs(", decision=ignore", stderr);
    std::fputc('\n', stderr);
}

void report_sub_reactor_log_emit_error(std::size_t reactor_id, const char* event) noexcept {
    std::fputs("sub reactor log emit failed: reactor_id=", stderr);
    try {
        const std::string reactor_id_text = std::to_string(reactor_id);
        std::fputs(reactor_id_text.c_str(), stderr);
    } catch (...) {
        std::fputs("unknown", stderr);
    }
    std::fputs(", event=", stderr);
    std::fputs(event != nullptr ? event : "unknown", stderr);
    std::fputs(", error=logger_emit_failed, decision=ignore", stderr);
    std::fputc('\n', stderr);
}

}  // namespace

HttpSubReactor::HttpSubReactor(std::size_t id, const ServerConfig& config, RequestDispatchFn dispatch_request,
                               LifecycleProviderFn lifecycle_provider, ForceCloseProviderFn force_close_provider,
                               FatalErrorFn fatal_error_callback)
    : id_(id),
      config_(config),
      dispatch_request_(std::move(dispatch_request)),
      lifecycle_provider_(std::move(lifecycle_provider)),
      force_close_provider_(std::move(force_close_provider)),
      fatal_error_callback_(std::move(fatal_error_callback)),
      events_(kDefaultEventCapacity) {}

HttpSubReactor::~HttpSubReactor() noexcept {
    try {
        shutdown();
    } catch (const std::exception& e) {
        report_destructor_cleanup_error("shutdown", e.what());
    } catch (...) {
        report_destructor_cleanup_error("shutdown", "unknown");
    }
}

bool HttpSubReactor::start() {
    shutdown();
    reset_runtime_state_for_start();

    if (!epoll_.open()) {
        const int err = errno;
        run_state_.store(RunState::Drained);
        common::Logger::instance()
            .error("sub reactor open epoll failed")
            .field("reactor_id", id_)
            .field("errno", err, common::errno_message(err));
        return false;
    }

    const int wakeup_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeup_fd < 0) {
        const int err = errno;
        epoll_.close();
        run_state_.store(RunState::Drained);
        common::Logger::instance()
            .error("sub reactor create wakeup fd failed")
            .field("reactor_id", id_)
            .field("errno", err, common::errno_message(err));
        return false;
    }

    if (!epoll_.add(wakeup_fd, kWakeupEpollEvents)) {
        const int err = errno;
        common::close_fd(wakeup_fd);
        epoll_.close();
        run_state_.store(RunState::Drained);
        common::Logger::instance()
            .error("sub reactor epoll add wakeup failed")
            .field("reactor_id", id_)
            .field("fd", wakeup_fd)
            .field("errno", err, common::errno_message(err));
        return false;
    }

    {
        std::lock_guard lock(wakeup_mutex_);
        wakeup_fd_ = wakeup_fd;
    }

    run_state_.store(RunState::Running);

    try {
        thread_ = std::thread(&HttpSubReactor::run_loop, this);
    } catch (const std::exception& e) {
        run_state_.store(RunState::Drained);
        {
            std::lock_guard lock(wakeup_mutex_);
            common::close_fd(wakeup_fd_);
            wakeup_fd_ = -1;
        }
        epoll_.close();
        common::Logger::instance()
            .error("sub reactor start thread failed")
            .field("reactor_id", id_)
            .field("error", e.what());
        return false;
    } catch (...) {
        run_state_.store(RunState::Drained);
        {
            std::lock_guard lock(wakeup_mutex_);
            common::close_fd(wakeup_fd_);
            wakeup_fd_ = -1;
        }
        epoll_.close();
        common::Logger::instance()
            .error("sub reactor start thread failed")
            .field("reactor_id", id_)
            .field("error", "unknown");
        return false;
    }

    return true;
}

std::size_t HttpSubReactor::shutdown() {
    run_state_.store(RunState::StopRequested);
    notify_wakeup();

    if (thread_.joinable()) {
        thread_.join();
    }

    close_all_connections();

    const std::size_t dropped_pending_count = clear_pending_queues();
    pending_accept_count_.store(0);
    pending_response_count_.store(0);

    {
        std::lock_guard lock(wakeup_mutex_);
        if (wakeup_fd_ >= 0) {
            common::close_fd(wakeup_fd_);
            wakeup_fd_ = -1;
        }
    }

    epoll_.close();

    connection_count_.store(0);
    pending_accept_count_.store(0);
    run_state_.store(RunState::Drained);

    return dropped_pending_count;
}

void HttpSubReactor::reset_runtime_state_for_start() {
    run_state_.store(RunState::Starting);
    connection_count_.store(0);
    pending_accept_count_.store(0);
    pending_response_count_.store(0);
    connections_.clear();
    clear_pending_queues();
}

std::size_t HttpSubReactor::clear_pending_queues() {
    std::vector<AcceptedConnection> pending_accepts;
    {
        std::lock_guard lock(pending_accepts_mutex_);
        pending_accepts.swap(pending_accepts_);
    }
    for (const AcceptedConnection& pending : pending_accepts) {
        common::close_fd(pending.fd);
    }

    std::size_t dropped_pending_count = 0;
    {
        std::lock_guard lock(pending_responses_mutex_);
        dropped_pending_count = pending_responses_.size();
        std::vector<PendingResponse>().swap(pending_responses_);
    }
    return dropped_pending_count;
}

void HttpSubReactor::close_all_connections() {
    std::vector<int> fds;
    fds.reserve(connections_.size());
    for (const auto& [fd, connection] : connections_) {
        fds.push_back(fd);
    }
    for (int fd : fds) {
        common::close_fd(fd);
    }
    connections_.clear();
}

void HttpSubReactor::request_stop() {
    RunState state = run_state_.load();
    while (state == RunState::Starting || state == RunState::Running) {
        if (run_state_.compare_exchange_weak(state, RunState::StopRequested)) {
            notify_wakeup();
            return;
        }
    }
}

void HttpSubReactor::notify_wakeup() {
    std::lock_guard lock(wakeup_mutex_);
    if (wakeup_fd_ < 0) {
        return;
    }

    const int err = net::notify_eventfd(wakeup_fd_);
    if (err != 0) {
        common::Logger::instance()
            .warn("sub reactor notify wakeup failed")
            .field("fd", wakeup_fd_)
            .field("reactor_id", id_)
            .field("errno", err, common::errno_message(err))
            .field("decision", "keep_running");
    }
}

bool HttpSubReactor::is_running() const {
    return run_state_.load() == RunState::Running;
}

bool HttpSubReactor::is_drained() const {
    return run_state_.load() == RunState::Drained;
}

std::size_t HttpSubReactor::connection_count() const {
    return connection_count_.load();
}

std::size_t HttpSubReactor::pending_accept_count() const {
    return pending_accept_count_.load();
}

std::size_t HttpSubReactor::pending_response_count() const {
    return pending_response_count_.load();
}

std::size_t HttpSubReactor::tracked_connection_count() const {
    return connection_count() + pending_accept_count();
}

LifecycleState HttpSubReactor::current_lifecycle_state() const {
    if (!lifecycle_provider_) {
        return LifecycleState::Idle;
    }
    return lifecycle_provider_();
}

int HttpSubReactor::load_wakeup_fd() const {
    std::lock_guard lock(wakeup_mutex_);
    return wakeup_fd_;
}

bool HttpSubReactor::should_keep_running_for_state(LifecycleState state) {
    return state == LifecycleState::Starting || state == LifecycleState::Running || state == LifecycleState::Stopping;
}

bool HttpSubReactor::handle_force_close_if_requested() {
    if (force_close_provider_ == nullptr || !force_close_provider_()) {
        return false;
    }

    std::vector<int> fds;
    fds.reserve(connections_.size());
    for (const auto& [fd, connection] : connections_) {
        fds.push_back(fd);
    }
    for (int fd : fds) {
        close_connection(fd, "graceful_shutdown_timeout");
    }
    return true;
}

bool HttpSubReactor::wait_and_process_events() {
    const int ready_count = epoll_.wait(events_, kEventWaitTimeoutMs);
    if (ready_count < 0) {
        if (errno == EINTR) {
            return true;
        }
        const int err = errno;
        common::Logger::instance()
            .error("sub reactor wait failed")
            .field("reactor_id", id_)
            .field("fd", epoll_.fd())
            .field("errno", err, common::errno_message(err))
            .field("decision", "stop_loop");
        notify_fatal_error_safely();
        return false;
    }

    process_ready_events(ready_count);
    sweep_idle_connections();
    return true;
}

void HttpSubReactor::run_loop() {
    try {
        bool shutdown_started = false;

        common::Logger::instance().debug("sub reactor started").field("reactor_id", id_);

        while (run_state_.load() == RunState::Running) {
            const LifecycleState state = current_lifecycle_state();
            if (!should_keep_running_for_state(state)) {
                break;
            }

            if (state == LifecycleState::Stopping) {
                begin_graceful_shutdown(shutdown_started);
                if (!has_inflight_work()) {
                    break;
                }
            }

            if (handle_force_close_if_requested()) {
                break;
            }

            if (!wait_and_process_events()) {
                break;
            }

            if (state == LifecycleState::Stopping && !has_inflight_work()) {
                break;
            }
        }

        run_state_.store(RunState::StopRequested);
        drain_pending_accepts();
        drain_pending_responses();
        run_state_.store(RunState::ThreadExited);

        common::Logger::instance()
            .debug("sub reactor stopped")
            .field("reactor_id", id_)
            .field("count", connections_.size());
    } catch (const std::exception& e) {
        handle_run_loop_exception(e.what());
    } catch (...) {
        handle_run_loop_exception("unknown");
    }
}

void HttpSubReactor::handle_run_loop_exception(const char* error) noexcept {
    run_state_.store(RunState::ThreadExited);

    try {
        common::Logger::instance()
            .error("sub reactor event loop exception")
            .field("reactor_id", id_)
            .field("error", error != nullptr ? error : "unknown")
            .field("decision", "stop_loop");
    } catch (...) {
        report_sub_reactor_log_emit_error(id_, "sub_reactor_event_loop_exception");
    }

    notify_fatal_error_safely();
}

void HttpSubReactor::notify_fatal_error_safely() noexcept {
    if (!fatal_error_callback_) {
        return;
    }

    try {
        fatal_error_callback_(id_);
    } catch (const std::exception& callback_error) {
        try {
            common::Logger::instance()
                .error("sub reactor fatal callback failed")
                .field("reactor_id", id_)
                .field("error", callback_error.what())
                .field("decision", "ignore");
        } catch (...) {
            report_sub_reactor_log_emit_error(id_, "sub_reactor_fatal_callback_failed");
        }
    } catch (...) {
        try {
            common::Logger::instance()
                .error("sub reactor fatal callback failed")
                .field("reactor_id", id_)
                .field("error", "unknown")
                .field("decision", "ignore");
        } catch (...) {
            report_sub_reactor_log_emit_error(id_, "sub_reactor_fatal_callback_failed");
        }
    }
}

void HttpSubReactor::begin_graceful_shutdown(bool& shutdown_started) {
    close_idle_connections_for_shutdown();
    if (shutdown_started) {
        return;
    }

    shutdown_started = true;
}

bool HttpSubReactor::has_inflight_work() {
    if (!connections_.empty()) {
        return true;
    }

    {
        std::lock_guard lock(pending_accepts_mutex_);
        if (!pending_accepts_.empty()) {
            return true;
        }
    }

    {
        std::lock_guard lock(pending_responses_mutex_);
        if (!pending_responses_.empty()) {
            return true;
        }
    }

    return false;
}

}  // namespace nebula::server
