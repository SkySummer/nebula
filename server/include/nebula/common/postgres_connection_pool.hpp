#ifndef NEBULA_COMMON_POSTGRES_CONNECTION_POOL_HPP
#define NEBULA_COMMON_POSTGRES_CONNECTION_POOL_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <pqxx/pqxx>
#include <string>
#include <string_view>

namespace nebula::common {

struct PostgresConnectionPoolOptions {
    std::string host;
    std::uint16_t port = 5432;
    std::string database;
    std::string user;
    std::string password;
    std::size_t max_connections = 4;
    std::int64_t connect_timeout_ms = 3000;
    std::int64_t acquire_timeout_ms = 3000;

    bool operator==(const PostgresConnectionPoolOptions&) const = default;
};

class PostgresConnectionPool {
private:
    struct PoolState;

#if defined(NEBULA_ENABLE_TESTING_API)
public:
    PostgresConnectionPool();
    ~PostgresConnectionPool() noexcept;
#else
private:
    PostgresConnectionPool();

public:
    ~PostgresConnectionPool() noexcept;
#endif

    PostgresConnectionPool(const PostgresConnectionPool&) = delete;
    PostgresConnectionPool& operator=(const PostgresConnectionPool&) = delete;
    PostgresConnectionPool(PostgresConnectionPool&&) = delete;
    PostgresConnectionPool& operator=(PostgresConnectionPool&&) = delete;

    class ConnectionLease {
    public:
        ConnectionLease(std::shared_ptr<PoolState> state, std::unique_ptr<pqxx::connection> connection);
        ~ConnectionLease() noexcept;

        ConnectionLease(const ConnectionLease&) = delete;
        ConnectionLease& operator=(const ConnectionLease&) = delete;
        ConnectionLease(ConnectionLease&& other) noexcept;
        ConnectionLease& operator=(ConnectionLease&&) = delete;

        [[nodiscard]] pqxx::connection& connection() const;

    private:
        std::shared_ptr<PoolState> state_;
        std::unique_ptr<pqxx::connection> connection_;
    };

    enum class InitializeStatus : std::uint8_t {
        Initialized,
        AlreadyInitialized,
        AlreadyInitializedWithDifferentOptions,
        InvalidConfig,
        ConnectionCreateFailed,
        ReplenishWorkerStartFailed,
    };

    enum class AcquireStatus : std::uint8_t {
        Acquired,
        TimedOut,
        PoolNotInitialized,
        PoolStopping,
    };

    struct AcquireResult {
        AcquireStatus status = AcquireStatus::TimedOut;
        std::optional<ConnectionLease> lease;
    };

    static PostgresConnectionPool& instance();

    [[nodiscard]] InitializeStatus initialize(PostgresConnectionPoolOptions options);
    [[nodiscard]] bool is_initialized();
    [[nodiscard]] AcquireResult acquire_connection();

#if defined(NEBULA_ENABLE_TESTING_API)
    void set_replenish_worker_start_failure_for_test(bool should_fail);
    [[nodiscard]] bool close_one_available_connection_for_test();
    [[nodiscard]] std::size_t connection_count_for_test();
#endif

private:
    static void release_connection_to_state(const std::shared_ptr<PoolState>& state,
                                            std::unique_ptr<pqxx::connection> connection);
    static bool prepare_replenish_attempt_locked(const std::shared_ptr<PoolState>& state,
                                                 PostgresConnectionPoolOptions& options);
    static void apply_replenish_result_locked(const std::shared_ptr<PoolState>& state,
                                              std::unique_ptr<pqxx::connection> replacement,
                                              bool& log_retry_with_backoff, std::int64_t& retry_backoff_ms);
    static void replenish_worker_loop(const std::shared_ptr<PoolState>& state);
    static bool ensure_replenish_worker_locked(const std::shared_ptr<PoolState>& state);
    static bool schedule_replenish_connection_locked(const std::shared_ptr<PoolState>& state);

    std::shared_ptr<PoolState> state_;
};

[[nodiscard]] std::string_view to_string(PostgresConnectionPool::InitializeStatus status) noexcept;

[[nodiscard]] std::string_view to_string(PostgresConnectionPool::AcquireStatus status) noexcept;

[[nodiscard]] std::string build_connection_info(const PostgresConnectionPoolOptions& options);

[[nodiscard]] std::optional<PostgresConnectionPool::ConnectionLease> acquire_connection_lease(
    std::string_view operation);

}  // namespace nebula::common

#endif  // NEBULA_COMMON_POSTGRES_CONNECTION_POOL_HPP
