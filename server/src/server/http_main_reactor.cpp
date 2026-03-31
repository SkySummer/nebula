#include "nebula/server/http_main_reactor.hpp"

#include <cerrno>

#include <sys/epoll.h>
#include <sys/eventfd.h>

#include "nebula/common/logger.hpp"
#include "nebula/common/posix_utils.hpp"
#include "nebula/net/eventfd_utils.hpp"
#include "nebula/server/reactor_constants.hpp"

namespace nebula::server {

HttpMainReactor::HttpMainReactor() : events_(kDefaultEventCapacity) {}

HttpMainReactor::~HttpMainReactor() noexcept {
    close();
}

bool HttpMainReactor::open(std::uint16_t port, int backlog) {
    close();

    if (!listener_.open(port, backlog)) {
        const int err = errno;
        common::Logger::instance()
            .error("open listener failed")
            .field("port", port)
            .field("errno", err, common::errno_message(err));
        return false;
    }

    if (!epoll_.open()) {
        const int err = errno;
        listener_.close();
        common::Logger::instance()
            .error("open epoll failed")
            .field("port", port)
            .field("errno", err, common::errno_message(err));
        return false;
    }

    const int wakeup_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeup_fd < 0) {
        const int err = errno;
        epoll_.close();
        listener_.close();
        common::Logger::instance()
            .error("create wakeup fd failed")
            .field("port", port)
            .field("errno", err, common::errno_message(err));
        return false;
    }

    if (!epoll_.add(listener_.fd(), kListenerEpollEvents)) {
        const int err = errno;
        const int listener_fd = listener_.fd();
        common::close_fd(wakeup_fd);
        epoll_.close();
        listener_.close();
        common::Logger::instance()
            .error("epoll add listener failed")
            .field("fd", listener_fd)
            .field("errno", err, common::errno_message(err));
        return false;
    }

    if (!epoll_.add(wakeup_fd, kWakeupEpollEvents)) {
        const int err = errno;
        common::close_fd(wakeup_fd);
        epoll_.close();
        listener_.close();
        common::Logger::instance()
            .error("epoll add wakeup failed")
            .field("fd", wakeup_fd)
            .field("errno", err, common::errno_message(err));
        return false;
    }

    {
        std::lock_guard lock(wakeup_mutex_);
        wakeup_fd_ = wakeup_fd;
    }

    return true;
}

void HttpMainReactor::close() noexcept {
    {
        std::lock_guard lock(wakeup_mutex_);
        if (wakeup_fd_ >= 0) {
            common::close_fd(wakeup_fd_);
            wakeup_fd_ = -1;
        }
    }

    epoll_.close();
    listener_.close();
}

bool HttpMainReactor::wait_and_process_events(const std::function<void()>& on_listener_ready, int timeout_ms) {
    const int ready_count = epoll_.wait(events_, timeout_ms);
    if (ready_count < 0) {
        if (errno == EINTR) {
            return true;
        }
        const int err = errno;
        common::Logger::instance()
            .error("epoll wait failed")
            .field("fd", epoll_.fd())
            .field("errno", err, common::errno_message(err))
            .field("decision", "stop_loop");
        return false;
    }

    for (int idx = 0; idx < ready_count; ++idx) {
        const epoll_event& event = events_[static_cast<std::size_t>(idx)];
        const int fd = event.data.fd;
        const int wakeup_fd = load_wakeup_fd();
        const int listener_fd = listener_.fd();

        if (listener_fd >= 0 && fd == listener_fd) {
            if (on_listener_ready) {
                on_listener_ready();
            }
            continue;
        }

        if (wakeup_fd >= 0 && fd == wakeup_fd) {
            drain_wakeup(wakeup_fd);
            continue;
        }

        common::Logger::instance().debug("unknown main event").field("fd", fd).field("events", event.events);
    }

    return true;
}

bool HttpMainReactor::close_listener_for_shutdown() {
    const int listener_fd = listener_.fd();
    if (listener_fd < 0) {
        return false;
    }

    if (!epoll_.del(listener_fd)) {
        const int err = errno;
        common::Logger::instance()
            .warn("epoll del listener failed")
            .field("fd", listener_fd)
            .field("errno", err, common::errno_message(err))
            .field("decision", "continue_shutdown");
    }

    listener_.close();
    common::Logger::instance()
        .info("listener closed for shutdown")
        .field("fd", listener_fd)
        .field("next_state", "draining");
    return true;
}

void HttpMainReactor::notify_wakeup() {
    std::lock_guard lock(wakeup_mutex_);
    if (wakeup_fd_ < 0) {
        return;
    }

    const int err = net::notify_eventfd(wakeup_fd_);
    if (err != 0) {
        common::Logger::instance()
            .warn("notify wakeup failed")
            .field("fd", wakeup_fd_)
            .field("errno", err, common::errno_message(err))
            .field("decision", "keep_running");
    }
}

net::AcceptedSocket HttpMainReactor::accept_one() const {
    return listener_.accept_one();
}

int HttpMainReactor::listener_fd() const {
    return listener_.fd();
}

std::uint16_t HttpMainReactor::listening_port() const {
    return listener_.port();
}

void HttpMainReactor::drain_wakeup(int wakeup_fd) {
    const int err = net::drain_eventfd(wakeup_fd);
    if (err != 0) {
        common::Logger::instance()
            .warn("drain wakeup failed")
            .field("fd", wakeup_fd)
            .field("errno", err, common::errno_message(err))
            .field("decision", "keep_running");
    }
}

int HttpMainReactor::load_wakeup_fd() const {
    std::lock_guard lock(wakeup_mutex_);
    return wakeup_fd_;
}

}  // namespace nebula::server
