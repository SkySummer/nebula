#ifndef NEBULA_NET_EVENTFD_UTILS_HPP
#define NEBULA_NET_EVENTFD_UTILS_HPP

#include <cerrno>
#include <cstdint>

#include <unistd.h>

#include "nebula/common/posix_utils.hpp"

namespace nebula::net {

inline int notify_eventfd(int fd) noexcept {
    const std::uint64_t one = 1;
    while (true) {
        const ssize_t write_n = ::write(fd, &one, sizeof(one));
        if (write_n > 0) {
            return 0;
        }
        if (write_n < 0 && errno == EINTR) {
            continue;
        }
        if (write_n < 0 && !common::is_would_block(errno)) {
            return errno;
        }
        return 0;
    }
}

inline int drain_eventfd(int fd) noexcept {
    std::uint64_t count = 0;
    while (true) {
        const ssize_t read_n = ::read(fd, &count, sizeof(count));
        if (read_n > 0) {
            continue;
        }
        if (read_n < 0 && errno == EINTR) {
            continue;
        }
        if (read_n < 0 && !common::is_would_block(errno)) {
            return errno;
        }
        return 0;
    }
}

}  // namespace nebula::net

#endif  // NEBULA_NET_EVENTFD_UTILS_HPP
