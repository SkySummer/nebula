#ifndef NEBULA_COMMON_PLATFORM_POSIX_HPP
#define NEBULA_COMMON_PLATFORM_POSIX_HPP

#include <cerrno>
#include <string>
#include <system_error>

#include <unistd.h>

namespace nebula::common {

inline bool is_would_block(int err) noexcept {
    return err == EAGAIN || err == EWOULDBLOCK;
}

inline void close_fd(int fd) noexcept {
    if (fd >= 0) {
        ::close(fd);
    }
}

inline std::string errno_message(int err) {
    if (err == 0) {
        return "unknown";
    }
    return std::system_category().message(err);
}

}  // namespace nebula::common

#endif  // NEBULA_COMMON_PLATFORM_POSIX_HPP
