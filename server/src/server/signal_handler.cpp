#include "nebula/server/signal_handler.hpp"

#include <array>
#include <cerrno>
#include <exception>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include "nebula/common/logger.hpp"
#include "nebula/common/posix_utils.hpp"

namespace nebula::server {

namespace {

std::string_view signal_name(int signal) {
    switch (signal) {
        case SIGINT:
            return "SIGINT";
        case SIGTERM:
            return "SIGTERM";
        default:
            return "UNKNOWN";
    }
}

std::mutex& signal_install_mutex() {
    static std::mutex mutex;
    return mutex;
}

volatile sig_atomic_t& active_signal_pipe_fd() {
    static volatile sig_atomic_t fd = -1;
    return fd;
}

void handle_termination_signal(int signal) {
    if (signal != SIGINT && signal != SIGTERM) {
        return;
    }

    const sig_atomic_t write_fd = active_signal_pipe_fd();
    if (write_fd < 0) {
        return;
    }

    const auto payload = static_cast<unsigned char>(signal);
    [[maybe_unused]] const ssize_t write_n = ::write(static_cast<int>(write_fd), &payload, sizeof(payload));
}

bool set_fd_nonblocking_and_cloexec(int fd) {
    int file_status_flags = ::fcntl(fd, F_GETFL, 0);
    if (file_status_flags < 0) {
        return false;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    if (::fcntl(fd, F_SETFL, file_status_flags | O_NONBLOCK) != 0) {
        return false;
    }

    int descriptor_flags = ::fcntl(fd, F_GETFD, 0);
    if (descriptor_flags < 0) {
        return false;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    if (::fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0) {
        return false;
    }

    return true;
}

}  // namespace

SignalHandler::SignalHandler() {
    init_signal_set();
}

SignalHandler::~SignalHandler() {
    stop();
}

bool SignalHandler::enabled() const {
    return signal_set_ready_ && signal_handlers_installed_;
}

void SignalHandler::init_signal_set() {
    if (signal_set_ready_) {
        return;
    }

    if (::sigemptyset(&signal_set_) != 0 || ::sigaddset(&signal_set_, SIGINT) != 0 ||
        ::sigaddset(&signal_set_, SIGTERM) != 0) {
        const int err = errno;
        common::Logger::instance()
            .warn("signal set init failed")
            .field("signal", "SIGINT,SIGTERM")
            .field("errno", err, common::errno_message(err))
            .field("fallback", "run_without_signal_shutdown");
        signal_set_ready_ = false;
        return;
    }

    signal_set_ready_ = true;
}

bool SignalHandler::ensure_signal_pipe() {
    if (signal_pipe_read_fd_ >= 0 && signal_pipe_write_fd_ >= 0) {
        return true;
    }

    std::array<int, 2> pipe_fds{-1, -1};
    if (::pipe(pipe_fds.data()) != 0) {
        const int err = errno;
        common::Logger::instance()
            .warn("signal pipe create failed")
            .field("signal", "SIGINT,SIGTERM")
            .field("errno", err, common::errno_message(err))
            .field("fallback", "run_without_signal_shutdown");
        return false;
    }

    const bool read_fd_configured = set_fd_nonblocking_and_cloexec(pipe_fds[0]);
    const bool write_fd_configured = set_fd_nonblocking_and_cloexec(pipe_fds[1]);
    if (!read_fd_configured || !write_fd_configured) {
        const int err = errno;
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        common::Logger::instance()
            .warn("signal pipe setup failed")
            .field("signal", "SIGINT,SIGTERM")
            .field("errno", err, common::errno_message(err))
            .field("fallback", "run_without_signal_shutdown");
        return false;
    }

    signal_pipe_read_fd_ = pipe_fds[0];
    signal_pipe_write_fd_ = pipe_fds[1];
    return true;
}

bool SignalHandler::install_signal_handlers() {
    if (signal_handlers_installed_) {
        return true;
    }

    std::lock_guard lock(signal_install_mutex());
    if (active_signal_pipe_fd() >= 0) {
        common::Logger::instance()
            .warn("signal handler install rejected")
            .field("signal", "SIGINT,SIGTERM")
            .field("error", "already_active")
            .field("fallback", "run_without_signal_shutdown");
        return false;
    }

    struct sigaction action{};
    action.sa_handler = handle_termination_signal;
    ::sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;

    active_signal_pipe_fd() = static_cast<sig_atomic_t>(signal_pipe_write_fd_);
    if (::sigaction(SIGINT, &action, &previous_sigint_action_) != 0) {
        const int err = errno;
        active_signal_pipe_fd() = -1;
        common::Logger::instance()
            .warn("signal handler install failed")
            .field("signal", "SIGINT")
            .field("errno", err, common::errno_message(err))
            .field("fallback", "run_without_signal_shutdown");
        return false;
    }
    if (::sigaction(SIGTERM, &action, &previous_sigterm_action_) != 0) {
        const int err = errno;
        ::sigaction(SIGINT, &previous_sigint_action_, nullptr);
        active_signal_pipe_fd() = -1;
        common::Logger::instance()
            .warn("signal handler install failed")
            .field("signal", "SIGTERM")
            .field("errno", err, common::errno_message(err))
            .field("fallback", "run_without_signal_shutdown");
        return false;
    }

    signal_handlers_installed_ = true;
    common::Logger::instance().info("termination signal handlers installed").field("signal", "SIGINT,SIGTERM");
    return true;
}

void SignalHandler::uninstall_signal_handlers() noexcept {
    if (!signal_handlers_installed_) {
        return;
    }

    std::lock_guard lock(signal_install_mutex());
    active_signal_pipe_fd() = -1;

    if (::sigaction(SIGINT, &previous_sigint_action_, nullptr) != 0) {
        const int err = errno;
        common::Logger::instance()
            .warn("signal handler restore failed")
            .field("signal", "SIGINT")
            .field("errno", err, common::errno_message(err))
            .field("decision", "keep_current_handler");
    }
    if (::sigaction(SIGTERM, &previous_sigterm_action_, nullptr) != 0) {
        const int err = errno;
        common::Logger::instance()
            .warn("signal handler restore failed")
            .field("signal", "SIGTERM")
            .field("errno", err, common::errno_message(err))
            .field("decision", "keep_current_handler");
    }

    signal_handlers_installed_ = false;
}

void SignalHandler::close_signal_pipe() noexcept {
    if (signal_pipe_read_fd_ >= 0) {
        ::close(signal_pipe_read_fd_);
        signal_pipe_read_fd_ = -1;
    }
    if (signal_pipe_write_fd_ >= 0) {
        ::close(signal_pipe_write_fd_);
        signal_pipe_write_fd_ = -1;
    }
}

void SignalHandler::wake_wait_loop() const noexcept {
    if (signal_pipe_write_fd_ < 0) {
        return;
    }

    constexpr unsigned char wake_payload = 0U;
    const ssize_t write_n = ::write(signal_pipe_write_fd_, &wake_payload, sizeof(wake_payload));
    if (write_n < 0) {
        const int err = errno;
        if (err == EINTR || err == EAGAIN || err == EWOULDBLOCK) {
            return;
        }

        common::Logger::instance()
            .warn("signal wake write failed")
            .field("fd", signal_pipe_write_fd_)
            .field("errno", err, common::errno_message(err))
            .field("decision", "wait_loop_poll_timeout");
    }
}

bool SignalHandler::handle_non_readable_events(int fd, std::int16_t events) noexcept {
    if ((events & (POLLERR | POLLNVAL)) != 0U) {
        common::Logger::instance()
            .warn("signal wait failed")
            .field("fd", fd)
            .field("events", events)
            .field("decision", "exit_wait_loop");
        return false;
    }
    return true;
}

bool SignalHandler::handle_signal_event(unsigned char signal, const std::function<void()>& stop_callback) noexcept {
    if (signal != static_cast<unsigned char>(SIGINT) && signal != static_cast<unsigned char>(SIGTERM)) {
        return true;
    }

    const int value = static_cast<int>(signal);
    common::Logger::instance()
        .info("termination signal received")
        .field("signal", value, signal_name(value))
        .field("decision", "request_server_stop");
    if (stop_callback) {
        stop_callback();
    }
    return false;
}

bool SignalHandler::drain_signal_pipe() noexcept {
    while (keep_waiting_.load()) {
        unsigned char signal = 0U;
        const ssize_t read_n = ::read(signal_pipe_read_fd_, &signal, sizeof(signal));
        if (read_n == 1) {
            if (!handle_signal_event(signal, stop_callback_)) {
                return false;
            }
            continue;
        }

        if (read_n == 0) {
            return true;
        }

        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }

        const int err = errno;
        common::Logger::instance()
            .warn("signal pipe read failed")
            .field("fd", signal_pipe_read_fd_)
            .field("errno", err, common::errno_message(err))
            .field("decision", "exit_wait_loop");
        return false;
    }

    return true;
}

void SignalHandler::start(std::function<void()> stop_callback) {
    if (!signal_set_ready_) {
        init_signal_set();
    }

    if (!signal_set_ready_ || signal_thread_.joinable()) {
        return;
    }

    if (!ensure_signal_pipe()) {
        return;
    }
    if (!install_signal_handlers()) {
        close_signal_pipe();
        return;
    }

    stop_callback_ = std::move(stop_callback);
    keep_waiting_.store(true);
    try {
        signal_thread_ = std::thread(&SignalHandler::wait_loop, this);
    } catch (const std::exception& e) {
        keep_waiting_.store(false);
        uninstall_signal_handlers();
        close_signal_pipe();
        common::Logger::instance()
            .warn("signal thread start failed")
            .field("error", e.what())
            .field("fallback", "run_without_signal_shutdown");
    } catch (...) {
        keep_waiting_.store(false);
        uninstall_signal_handlers();
        close_signal_pipe();
        common::Logger::instance()
            .warn("signal thread start failed")
            .field("error", "unknown")
            .field("fallback", "run_without_signal_shutdown");
    }
}

void SignalHandler::stop() noexcept {
    keep_waiting_.store(false);
    wake_wait_loop();
    if (signal_thread_.joinable()) {
        signal_thread_.join();
    }
    uninstall_signal_handlers();
    close_signal_pipe();
    stop_callback_ = {};
}

void SignalHandler::wait_loop() noexcept {
    constexpr int signal_wait_poll_ms = 200;
    pollfd descriptor{};
    descriptor.fd = signal_pipe_read_fd_;
    descriptor.events = POLLIN;

    while (keep_waiting_.load()) {
        descriptor.revents = 0;
        const int ready = ::poll(&descriptor, 1, signal_wait_poll_ms);
        if (ready == 0) {
            continue;
        }

        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            const int err = errno;
            common::Logger::instance()
                .warn("signal wait failed")
                .field("signal", "SIGINT,SIGTERM")
                .field("timeout_ms", signal_wait_poll_ms)
                .field("errno", err, common::errno_message(err))
                .field("decision", "exit_wait_loop");
            return;
        }

        if ((descriptor.revents & POLLIN) == 0U) {
            if (!handle_non_readable_events(descriptor.fd, static_cast<std::int16_t>(descriptor.revents))) {
                return;
            }
            continue;
        }

        if (!drain_signal_pipe()) {
            return;
        }
    }
}

}  // namespace nebula::server
