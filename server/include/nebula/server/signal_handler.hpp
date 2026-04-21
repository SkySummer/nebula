#ifndef NEBULA_SERVER_SIGNAL_HANDLER_HPP
#define NEBULA_SERVER_SIGNAL_HANDLER_HPP

#include <atomic>
#include <csignal>
#include <cstdint>
#include <functional>
#include <thread>

namespace nebula::server {

class SignalHandler {
public:
    SignalHandler();
    ~SignalHandler() noexcept;

    SignalHandler(const SignalHandler&) = delete;
    SignalHandler& operator=(const SignalHandler&) = delete;
    SignalHandler(SignalHandler&&) = delete;
    SignalHandler& operator=(SignalHandler&&) = delete;

    [[nodiscard]] bool enabled() const;
    void start(std::function<void()> stop_callback);
    void stop() noexcept;

private:
    void init_signal_set();
    bool ensure_signal_pipe();
    bool install_signal_handlers();
    void uninstall_signal_handlers() noexcept;
    void close_signal_pipe() noexcept;
    void wake_wait_loop() const noexcept;
    static bool handle_non_readable_events(int fd, std::int16_t events) noexcept;
    static bool handle_signal_event(unsigned char signal, const std::function<void()>& stop_callback) noexcept;
    bool drain_signal_pipe() noexcept;
    void wait_loop() noexcept;

    sigset_t signal_set_{};
    std::atomic<bool> keep_waiting_ = false;
    std::thread thread_;
    struct sigaction previous_sigint_action_{};
    struct sigaction previous_sigterm_action_{};
    int signal_pipe_read_fd_ = -1;
    int signal_pipe_write_fd_ = -1;
    bool signal_set_ready_ = false;
    bool signal_handlers_installed_ = false;
    std::function<void()> stop_callback_;
};

}  // namespace nebula::server

#endif  // NEBULA_SERVER_SIGNAL_HANDLER_HPP
