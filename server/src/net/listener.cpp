#include "nebula/net/listener.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <format>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace nebula::net {

namespace {

sockaddr* as_sockaddr(sockaddr_in& addr) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<sockaddr*>(&addr);
}

std::string format_peer(const sockaddr_in& addr) {
    std::array<char, INET_ADDRSTRLEN> ip{};
    const char* result = ::inet_ntop(AF_INET, &addr.sin_addr, ip.data(), ip.size());
    if (result == nullptr) {
        return "unknown:0";
    }
    return std::format("{}:{}", result, ntohs(addr.sin_port));
}

}  // namespace

Listener::~Listener() noexcept {
    close();
}

Listener::Listener(Listener&& other) noexcept : fd_(other.fd_), port_(other.port_) {
    other.fd_ = -1;
    other.port_ = 0;
}

Listener& Listener::operator=(Listener&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    close();
    fd_ = other.fd_;
    port_ = other.port_;
    other.fd_ = -1;
    other.port_ = 0;
    return *this;
}

bool Listener::open(std::uint16_t port, int backlog) {
    close();

    fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
        return false;
    }

    int reuse = 1;
    if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        close();
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (::bind(fd_, as_sockaddr(addr), sizeof(addr)) != 0) {
        close();
        return false;
    }

    if (::listen(fd_, backlog) != 0) {
        close();
        return false;
    }

    sockaddr_in bound_addr{};
    socklen_t bound_len = sizeof(bound_addr);
    if (::getsockname(fd_, as_sockaddr(bound_addr), &bound_len) != 0) {
        close();
        return false;
    }

    port_ = ntohs(bound_addr.sin_port);
    return true;
}

void Listener::close() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    port_ = 0;
}

int Listener::fd() const {
    return fd_;
}

std::uint16_t Listener::port() const {
    return port_;
}

AcceptedSocket Listener::accept_one() const {
    if (fd_ < 0) {
        return AcceptedSocket{};
    }

    sockaddr_in peer_addr{};
    socklen_t peer_len = sizeof(peer_addr);
    int accepted_fd = ::accept4(fd_, as_sockaddr(peer_addr), &peer_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (accepted_fd < 0) {
        return AcceptedSocket{};
    }

    return {.fd = accepted_fd, .peer = format_peer(peer_addr)};
}

}  // namespace nebula::net
