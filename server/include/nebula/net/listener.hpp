#ifndef NEBULA_NET_LISTENER_HPP
#define NEBULA_NET_LISTENER_HPP

#include <cstdint>
#include <string>

namespace nebula::net {

struct AcceptedSocket {
    int fd = -1;
    std::string peer;
};

class Listener {
public:
    Listener() = default;
    ~Listener();

    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    Listener(Listener&& other) noexcept;
    Listener& operator=(Listener&& other) noexcept;

    bool open(std::uint16_t port, int backlog);
    void close();

    [[nodiscard]] int fd() const;
    [[nodiscard]] std::uint16_t port() const;

    [[nodiscard]] AcceptedSocket accept_one() const;

private:
    int fd_ = -1;
    std::uint16_t port_ = 0;
};

}  // namespace nebula::net

#endif  // NEBULA_NET_LISTENER_HPP
