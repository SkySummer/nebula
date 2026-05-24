#ifndef NEBULA_NET_EPOLL_LOOP_HPP
#define NEBULA_NET_EPOLL_LOOP_HPP

#include <cstdint>
#include <span>

#include <sys/epoll.h>

namespace nebula::net {

class EpollLoop {
public:
    EpollLoop() = default;
    ~EpollLoop() noexcept;

    EpollLoop(const EpollLoop&) = delete;
    EpollLoop& operator=(const EpollLoop&) = delete;
    EpollLoop(EpollLoop&& other) noexcept;
    EpollLoop& operator=(EpollLoop&& other) noexcept;

    bool open();
    void close() noexcept;

    [[nodiscard]] int fd() const;

    [[nodiscard]] bool ctl(int op, int fd, std::uint32_t events) const;
    [[nodiscard]] bool ctl(int op, int fd) const;

    [[nodiscard]] bool add(int fd, std::uint32_t events) const;
    [[nodiscard]] bool mod(int fd, std::uint32_t events) const;
    [[nodiscard]] bool del(int fd) const;

    [[nodiscard]] int wait(std::span<epoll_event> out_events, int timeout_ms) const;

private:
    int fd_ = -1;
};

}  // namespace nebula::net

#endif  // NEBULA_NET_EPOLL_LOOP_HPP
