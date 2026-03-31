#ifndef NEBULA_SERVER_HTTP_MAIN_REACTOR_HPP
#define NEBULA_SERVER_HTTP_MAIN_REACTOR_HPP

#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "nebula/net/epoll_loop.hpp"
#include "nebula/net/listener.hpp"

namespace nebula::server {

class HttpMainReactor {
public:
    HttpMainReactor();
    ~HttpMainReactor() noexcept;

    HttpMainReactor(const HttpMainReactor&) = delete;
    HttpMainReactor& operator=(const HttpMainReactor&) = delete;
    HttpMainReactor(HttpMainReactor&&) = delete;
    HttpMainReactor& operator=(HttpMainReactor&&) = delete;

    bool open(std::uint16_t port, int backlog);
    void close() noexcept;
    [[nodiscard]] bool wait_and_process_events(const std::function<void()>& on_listener_ready, int timeout_ms);
    [[nodiscard]] bool close_listener_for_shutdown();

    void notify_wakeup();

    [[nodiscard]] net::AcceptedSocket accept_one() const;
    [[nodiscard]] int listener_fd() const;
    [[nodiscard]] std::uint16_t listening_port() const;

private:
    static void drain_wakeup(int wakeup_fd);
    [[nodiscard]] int load_wakeup_fd() const;

    net::Listener listener_;
    net::EpollLoop epoll_;
    std::vector<epoll_event> events_;

    mutable std::mutex wakeup_mutex_;
    int wakeup_fd_ = -1;
};

}  // namespace nebula::server

#endif  // NEBULA_SERVER_HTTP_MAIN_REACTOR_HPP
