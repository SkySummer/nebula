#ifndef NEBULA_COMMON_POSIX_UTILS_HPP
#define NEBULA_COMMON_POSIX_UTILS_HPP

#include <cerrno>
#include <cstring>
#include <string>

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
    const char* text = std::strerror(err);
    if (text == nullptr) {
        return "unknown";
    }
    return text;
}

}  // namespace nebula::common

#endif  // NEBULA_COMMON_POSIX_UTILS_HPP
