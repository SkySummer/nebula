#include "nebula/net/epoll_loop.hpp"

#include <sys/epoll.h>
#include <unistd.h>

namespace nebula::net {

EpollLoop::~EpollLoop() noexcept {
    close();
}

EpollLoop::EpollLoop(EpollLoop&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
}

EpollLoop& EpollLoop::operator=(EpollLoop&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    close();
    fd_ = other.fd_;
    other.fd_ = -1;
    return *this;
}

bool EpollLoop::open() {
    close();

    fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    return fd_ >= 0;
}

void EpollLoop::close() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

int EpollLoop::fd() const {
    return fd_;
}

bool EpollLoop::ctl(int op, int fd, std::uint32_t events) const {
    if (fd_ < 0) {
        return false;
    }

    epoll_event event{};
    event.events = events;
    event.data.fd = fd;
    return ::epoll_ctl(fd_, op, fd, &event) == 0;
}

bool EpollLoop::ctl(int op, int fd) const {
    if (fd_ < 0) {
        return false;
    }
    return ::epoll_ctl(fd_, op, fd, nullptr) == 0;
}

bool EpollLoop::add(int fd, std::uint32_t events) const {
    return ctl(EPOLL_CTL_ADD, fd, events);
}

bool EpollLoop::mod(int fd, std::uint32_t events) const {
    return ctl(EPOLL_CTL_MOD, fd, events);
}

bool EpollLoop::del(int fd) const {
    return ctl(EPOLL_CTL_DEL, fd);
}

int EpollLoop::wait(std::span<epoll_event> out_events, int timeout_ms) const {
    if (fd_ < 0 || out_events.empty()) {
        return -1;
    }
    return ::epoll_wait(fd_, out_events.data(), static_cast<int>(out_events.size()), timeout_ms);
}

}  // namespace nebula::net
