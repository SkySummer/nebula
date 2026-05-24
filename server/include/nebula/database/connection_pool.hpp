#ifndef NEBULA_DATABASE_CONNECTION_POOL_HPP
#define NEBULA_DATABASE_CONNECTION_POOL_HPP

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <pqxx/pqxx>
#include <source_location>
#include <stop_token>
#include <thread>
#include <vector>

#include "nebula/database/config.hpp"

namespace nebula::database {

class ConnectionPool;

class ConnectionLease {
public:
    ~ConnectionLease() noexcept;

    ConnectionLease(const ConnectionLease&) = delete;
    ConnectionLease& operator=(const ConnectionLease&) = delete;
    ConnectionLease(ConnectionLease&& other) = default;
    ConnectionLease& operator=(ConnectionLease&&) = delete;

    [[nodiscard]] pqxx::connection& connection() const;

private:
    friend class ConnectionPool;

    ConnectionLease(std::shared_ptr<ConnectionPool> pool, std::unique_ptr<pqxx::connection> connection);

    std::shared_ptr<ConnectionPool> pool_;
    std::unique_ptr<pqxx::connection> connection_;
};

class ConnectionPool : public std::enable_shared_from_this<ConnectionPool> {
public:
    [[nodiscard]] static std::shared_ptr<ConnectionPool> create(const DatabaseConfig& config);

    ~ConnectionPool() noexcept;

    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;
    ConnectionPool(ConnectionPool&&) = delete;
    ConnectionPool& operator=(ConnectionPool&&) = delete;

    [[nodiscard]] std::optional<ConnectionLease> acquire_lease(
        std::source_location location = std::source_location::current());

private:
    friend class ConnectionLease;

    explicit ConnectionPool(DatabaseConfig config);

    enum class PoolState : std::uint8_t {
        Running,
        Stopping,
    };

    void release_connection(std::unique_ptr<pqxx::connection> connection);

    [[nodiscard]] std::size_t connection_count_locked() const noexcept;
    bool prepare_replenish_attempt_locked();
    void request_replenish_locked();
    void apply_replenish_result(std::unique_ptr<pqxx::connection> replacement, bool& log_retry_with_backoff,
                                std::chrono::milliseconds& retry_backoff);
    void replenish_worker_loop(const std::stop_token& stop);
    bool schedule_replenish_connection_locked();

    const DatabaseConfig config_;
    std::mutex mutex_;
    std::condition_variable_any condition_;
    std::vector<std::unique_ptr<pqxx::connection>> available_connections_;
    std::size_t in_use_connections_ = 0U;
    PoolState state_ = PoolState::Running;
    bool replenish_in_progress_ = false;
    bool replenish_requested_ = false;
    std::chrono::steady_clock::time_point next_replenish_retry_at_;
    std::chrono::milliseconds replenish_retry_backoff_;
    std::jthread replenish_worker_;
};

}  // namespace nebula::database

#endif  // NEBULA_DATABASE_CONNECTION_POOL_HPP
