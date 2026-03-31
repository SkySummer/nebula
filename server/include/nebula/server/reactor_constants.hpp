#ifndef NEBULA_SERVER_REACTOR_CONSTANTS_HPP
#define NEBULA_SERVER_REACTOR_CONSTANTS_HPP

#include <cstddef>
#include <cstdint>

#include <sys/epoll.h>

namespace nebula::server {

constexpr int kEventWaitTimeoutMs = 100;
constexpr std::size_t kDefaultEventCapacity = 128U;
constexpr std::uint32_t kListenerEpollEvents = EPOLLIN | EPOLLET;
constexpr std::uint32_t kWakeupEpollEvents = EPOLLIN | EPOLLET;
constexpr std::uint32_t kConnectionReadEvents = EPOLLIN | EPOLLET;
constexpr std::uint32_t kConnectionReadWriteEvents = EPOLLIN | EPOLLOUT | EPOLLET;

}  // namespace nebula::server

#endif  // NEBULA_SERVER_REACTOR_CONSTANTS_HPP
