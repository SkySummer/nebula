#include "nebula/common/postgres_connection_pool.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "nebula/common/logger.hpp"

namespace nebula::common {

struct PostgresConnectionPool::PoolState {
    PostgresConnectionPoolOptions options;
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<std::unique_ptr<pqxx::connection>> available_connections;
    std::jthread replenish_worker;
    std::size_t in_use_connections = 0U;
    bool initialized = false;
    bool shutting_down = false;
    bool replenish_in_progress = false;
    bool replenish_requested = false;
    std::chrono::steady_clock::time_point next_replenish_retry_at = std::chrono::steady_clock::now();
    std::int64_t replenish_retry_backoff_ms = 100;
    bool fail_replenish_worker_start_for_test = false;
};

namespace {

constexpr std::int64_t kReplenishRetryBackoffInitialMs = 100;
constexpr std::int64_t kReplenishRetryBackoffMaxMs = 2000;

std::string quote_conninfo_value(std::string_view value) {
    std::string quoted;
    quoted.reserve(value.size() + 2U);
    quoted.push_back('\'');
    for (const char ch : value) {
        if (ch == '\\' || ch == '\'') {
            quoted.push_back('\\');
        }
        quoted.push_back(ch);
    }
    quoted.push_back('\'');
    return quoted;
}

void log_acquire_timeout(std::int64_t acquire_timeout_ms, std::size_t max_connections, std::size_t in_use,
                         std::size_t available) {
    Logger::instance()
        .warn(LogDomain::Common, "postgres connection acquire timed out")
        .field("timeout_ms", acquire_timeout_ms)
        .field("max_connections", max_connections)
        .field("in_use_connections", in_use)
        .field("available_connections", available)
        .field("decision", "return_timed_out");
}

std::unique_ptr<pqxx::connection> create_connection(const PostgresConnectionPoolOptions& options,
                                                    bool log_failure = true) {
    try {
        auto connection = std::make_unique<pqxx::connection>(build_connection_info(options));
        if (!connection->is_open()) {
            if (log_failure) {
                Logger::instance()
                    .error(LogDomain::Common, "postgres connection create failed")
                    .field("error", "connection_not_open")
                    .field("decision", "return_nullptr");
            }
            return nullptr;
        }
        return connection;
    } catch (const std::exception& e) {
        if (log_failure) {
            Logger::instance()
                .error(LogDomain::Common, "postgres connection create failed")
                .field("error", e.what())
                .field("decision", "return_nullptr");
        }
        return nullptr;
    }
}

}  // namespace

std::string build_connection_info(const PostgresConnectionPoolOptions& options) {
    const std::int64_t timeout_seconds = std::clamp<std::int64_t>(options.connect_timeout_ms / 1000, 1, 60);

    std::string info;
    info.reserve(256);
    info.append("host=").append(quote_conninfo_value(options.host));
    info.append(" port=").append(std::to_string(options.port));
    info.append(" dbname=").append(quote_conninfo_value(options.database));
    info.append(" user=").append(quote_conninfo_value(options.user));
    info.append(" password=").append(quote_conninfo_value(options.password));
    info.append(" connect_timeout=").append(std::to_string(timeout_seconds));
    return info;
}

std::string_view to_string(PostgresConnectionPool::InitializeStatus status) noexcept {
    switch (status) {
        case PostgresConnectionPool::InitializeStatus::Initialized:
            return "initialized";
        case PostgresConnectionPool::InitializeStatus::AlreadyInitialized:
            return "already_initialized";
        case PostgresConnectionPool::InitializeStatus::AlreadyInitializedWithDifferentOptions:
            return "already_initialized_with_different_options";
        case PostgresConnectionPool::InitializeStatus::InvalidConfig:
            return "invalid_config";
        case PostgresConnectionPool::InitializeStatus::ConnectionCreateFailed:
            return "connection_create_failed";
        case PostgresConnectionPool::InitializeStatus::ReplenishWorkerStartFailed:
            return "replenish_worker_start_failed";
    }
    return "unknown";
}

std::string_view to_string(PostgresConnectionPool::AcquireStatus status) noexcept {
    switch (status) {
        case PostgresConnectionPool::AcquireStatus::Acquired:
            return "acquired";
        case PostgresConnectionPool::AcquireStatus::TimedOut:
            return "timed_out";
        case PostgresConnectionPool::AcquireStatus::PoolNotInitialized:
            return "pool_not_initialized";
        case PostgresConnectionPool::AcquireStatus::PoolStopping:
            return "pool_stopping";
    }
    return "unknown";
}

PostgresConnectionPool::PostgresConnectionPool() : state_(std::make_shared<PoolState>()) {}

PostgresConnectionPool::ConnectionLease::ConnectionLease(std::shared_ptr<PoolState> state,
                                                         std::unique_ptr<pqxx::connection> connection)
    : state_(std::move(state)), connection_(std::move(connection)) {}

PostgresConnectionPool::ConnectionLease::ConnectionLease(ConnectionLease&& other) noexcept
    : state_(std::move(other.state_)), connection_(std::move(other.connection_)) {}

PostgresConnectionPool::ConnectionLease::~ConnectionLease() noexcept {
    if (!state_ || !connection_) {
        return;
    }
    try {
        PostgresConnectionPool::release_connection_to_state(state_, std::move(connection_));
    } catch (const std::exception& e) {
        Logger::instance()
            .error(LogDomain::Common, "postgres connection lease destructor release failed")
            .field("error", e.what())
            .field("decision", "ignore");
    } catch (...) {
        Logger::instance()
            .error(LogDomain::Common, "postgres connection lease destructor release failed")
            .field("error", "unknown")
            .field("decision", "ignore");
    }
}

pqxx::connection& PostgresConnectionPool::ConnectionLease::connection() const {
    return *connection_;
}

PostgresConnectionPool& PostgresConnectionPool::instance() {
    [[maybe_unused]] static Logger& logger = Logger::instance();
    static PostgresConnectionPool pool;
    return pool;
}

PostgresConnectionPool::~PostgresConnectionPool() noexcept {
    const std::shared_ptr<PoolState> state = state_;
    std::jthread replenish_worker;
    try {
        {
            std::lock_guard lock(state->mutex);
            state->shutting_down = true;
            state->initialized = false;
            state->replenish_in_progress = false;
            state->replenish_requested = false;
            replenish_worker = std::move(state->replenish_worker);
        }
        state->condition.notify_all();
        if (replenish_worker.joinable()) {
            replenish_worker.join();
        }
    } catch (const std::exception& e) {
        Logger::instance()
            .error(LogDomain::Common, "postgres connection pool destructor cleanup failed")
            .field("error", e.what())
            .field("decision", "ignore");
    } catch (...) {
        Logger::instance()
            .error(LogDomain::Common, "postgres connection pool destructor cleanup failed")
            .field("error", "unknown")
            .field("decision", "ignore");
    }
}

PostgresConnectionPool::InitializeStatus PostgresConnectionPool::initialize(PostgresConnectionPoolOptions options) {
    if (options.max_connections == 0U) {
        Logger::instance()
            .error(LogDomain::Common, "postgres connection pool init failed")
            .field("error", "invalid_max_connections")
            .field("decision", "stop_init");
        return InitializeStatus::InvalidConfig;
    }

    if (options.acquire_timeout_ms <= 0) {
        Logger::instance()
            .error(LogDomain::Common, "postgres connection pool init failed")
            .field("error", "invalid_acquire_timeout_ms")
            .field("decision", "stop_init");
        return InitializeStatus::InvalidConfig;
    }

    if (options.connect_timeout_ms <= 0) {
        Logger::instance()
            .error(LogDomain::Common, "postgres connection pool init failed")
            .field("error", "invalid_connect_timeout_ms")
            .field("decision", "stop_init");
        return InitializeStatus::InvalidConfig;
    }

    const std::shared_ptr<PoolState> state = state_;
    std::lock_guard lock(state->mutex);
    if (state->initialized) {
        if (state->options == options) {
            return InitializeStatus::AlreadyInitialized;
        }

        Logger::instance()
            .error(LogDomain::Common, "postgres connection pool init failed")
            .field("error", "already_initialized_with_different_options")
            .field("decision", "keep_existing_pool");
        return InitializeStatus::AlreadyInitializedWithDifferentOptions;
    }

    state->options = std::move(options);
    state->shutting_down = false;
    state->in_use_connections = 0U;
    state->replenish_in_progress = false;
    state->replenish_requested = false;
    state->next_replenish_retry_at = std::chrono::steady_clock::now();
    state->replenish_retry_backoff_ms = kReplenishRetryBackoffInitialMs;
    state->available_connections.clear();
    state->available_connections.reserve(state->options.max_connections);
    for (std::size_t idx = 0; idx < state->options.max_connections; ++idx) {
        auto connection = create_connection(state->options);
        if (!connection) {
            Logger::instance()
                .error(LogDomain::Common, "postgres connection pool init failed")
                .field("count", state->available_connections.size())
                .field("max_connections", state->options.max_connections)
                .field("decision", "stop_init");
            state->available_connections.clear();
            return InitializeStatus::ConnectionCreateFailed;
        }
        state->available_connections.push_back(std::move(connection));
    }
    if (!ensure_replenish_worker_locked(state)) {
        Logger::instance()
            .error(LogDomain::Common, "postgres connection pool init failed")
            .field("error", "replenish_worker_start_failed")
            .field("decision", "stop_init");
        state->available_connections.clear();
        state->replenish_in_progress = false;
        state->replenish_requested = false;
        return InitializeStatus::ReplenishWorkerStartFailed;
    }

    state->initialized = true;

    Logger::instance()
        .info(LogDomain::Common, "postgres connection pool initialized")
        .field("count", state->available_connections.size())
        .field("max_connections", state->options.max_connections);
    return InitializeStatus::Initialized;
}

bool PostgresConnectionPool::is_initialized() {
    const std::shared_ptr<PoolState> state = state_;
    std::lock_guard lock(state->mutex);
    return state->initialized;
}

#if defined(NEBULA_ENABLE_TESTING_API)
void PostgresConnectionPool::set_replenish_worker_start_failure_for_test(const bool should_fail) {
    const std::shared_ptr<PoolState> state = state_;
    std::lock_guard lock(state->mutex);
    state->fail_replenish_worker_start_for_test = should_fail;
}

bool PostgresConnectionPool::close_one_available_connection_for_test() {
    const std::shared_ptr<PoolState> state = state_;
    std::lock_guard lock(state->mutex);
    if (state->available_connections.empty()) {
        return false;
    }

    auto& connection = state->available_connections.back();
    if (!connection) {
        return false;
    }

    try {
        if (connection->is_open()) {
            connection->close();
        }
    } catch (const std::exception& e) {
        Logger::instance()
            .warn(LogDomain::Common, "postgres connection close for test failed")
            .field("error", e.what())
            .field("decision", "return_false");
        return false;
    } catch (...) {
        Logger::instance()
            .warn(LogDomain::Common, "postgres connection close for test failed")
            .field("error", "unknown")
            .field("decision", "return_false");
        return false;
    }
    return true;
}

std::size_t PostgresConnectionPool::connection_count_for_test() {
    const std::shared_ptr<PoolState> state = state_;
    std::lock_guard lock(state->mutex);
    return state->available_connections.size() + state->in_use_connections;
}
#endif

PostgresConnectionPool::AcquireResult PostgresConnectionPool::acquire_connection() {
    const std::shared_ptr<PoolState> state = state_;
    std::unique_lock lock(state->mutex);
    const std::int64_t acquire_timeout_ms = state->options.acquire_timeout_ms;
    const std::size_t max_connections = state->options.max_connections;
    const auto timeout = std::chrono::milliseconds(acquire_timeout_ms);
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (true) {
        if (!state->initialized) {
            Logger::instance()
                .warn(LogDomain::Common, "postgres connection acquire failed")
                .field("error", "pool_not_initialized")
                .field("decision", "return_not_initialized");
            return {.status = AcquireStatus::PoolNotInitialized, .lease = std::nullopt};
        }

        if (state->shutting_down) {
            Logger::instance()
                .warn(LogDomain::Common, "postgres connection acquire failed")
                .field("error", "pool_stopping")
                .field("decision", "return_pool_stopping");
            return {.status = AcquireStatus::PoolStopping, .lease = std::nullopt};
        }

        if (!state->available_connections.empty()) {
            auto connection = std::move(state->available_connections.back());
            state->available_connections.pop_back();
            ++state->in_use_connections;

            if (connection && connection->is_open()) {
                return {
                    .status = AcquireStatus::Acquired,
                    .lease = std::optional<ConnectionLease>(std::in_place, state, std::move(connection)),
                };
            }

            if (state->in_use_connections > 0U) {
                --state->in_use_connections;
            }
            schedule_replenish_connection_locked(state);
            Logger::instance()
                .warn(LogDomain::Common, "postgres connection unhealthy")
                .field("error", "connection_not_open")
                .field("decision", "discard_connection");
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            log_acquire_timeout(acquire_timeout_ms, max_connections, state->in_use_connections,
                                state->available_connections.size());
            return {.status = AcquireStatus::TimedOut, .lease = std::nullopt};
        }

        const std::size_t connection_count = state->available_connections.size() + state->in_use_connections;
        const bool should_retry_replenish = connection_count < max_connections;
        if (should_retry_replenish) {
            schedule_replenish_connection_locked(state);
        }
        state->condition.wait_until(lock, deadline);
    }
}

bool PostgresConnectionPool::prepare_replenish_attempt_locked(const std::shared_ptr<PoolState>& state,
                                                              PostgresConnectionPoolOptions& options) {
    if (!state) {
        return false;
    }
    if (state->shutting_down) {
        return false;
    }
    if (!state->initialized || !state->replenish_requested || state->replenish_in_progress) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now < state->next_replenish_retry_at) {
        return false;
    }

    const std::size_t connection_count = state->available_connections.size() + state->in_use_connections;
    if (connection_count >= state->options.max_connections) {
        state->replenish_requested = false;
        state->replenish_retry_backoff_ms = kReplenishRetryBackoffInitialMs;
        return false;
    }

    state->replenish_in_progress = true;
    options = state->options;
    return true;
}

void PostgresConnectionPool::apply_replenish_result_locked(const std::shared_ptr<PoolState>& state,
                                                           std::unique_ptr<pqxx::connection> replacement,
                                                           bool& log_retry_with_backoff,
                                                           std::int64_t& retry_backoff_ms) {
    if (!state) {
        return;
    }

    state->replenish_in_progress = false;
    const std::size_t current_count = state->available_connections.size() + state->in_use_connections;
    if (!state->initialized || state->shutting_down || current_count >= state->options.max_connections) {
        state->replenish_requested = false;
        state->replenish_retry_backoff_ms = kReplenishRetryBackoffInitialMs;
        return;
    }

    if (replacement) {
        state->available_connections.push_back(std::move(replacement));
        const std::size_t replenished_count = state->available_connections.size() + state->in_use_connections;
        state->replenish_requested = replenished_count < state->options.max_connections;
        state->next_replenish_retry_at = std::chrono::steady_clock::now();
        state->replenish_retry_backoff_ms = kReplenishRetryBackoffInitialMs;
        return;
    }

    log_retry_with_backoff = true;
    retry_backoff_ms = state->replenish_retry_backoff_ms;
    state->replenish_requested = true;
    state->next_replenish_retry_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(retry_backoff_ms);
    state->replenish_retry_backoff_ms = std::min<std::int64_t>(kReplenishRetryBackoffMaxMs, retry_backoff_ms * 2);
}

void PostgresConnectionPool::replenish_worker_loop(const std::shared_ptr<PoolState>& state) {
    if (!state) {
        return;
    }

    while (true) {
        PostgresConnectionPoolOptions options;

        {
            std::unique_lock lock(state->mutex);
            while (!prepare_replenish_attempt_locked(state, options)) {
                if (state->shutting_down) {
                    return;
                }
                const bool wait_for_retry_backoff = state->initialized && state->replenish_requested &&
                                                    !state->replenish_in_progress &&
                                                    std::chrono::steady_clock::now() < state->next_replenish_retry_at;
                if (wait_for_retry_backoff) {
                    state->condition.wait_until(lock, state->next_replenish_retry_at);
                } else {
                    state->condition.wait(lock);
                }
            }
        }

        auto replacement = create_connection(options, false);

        bool log_retry_with_backoff = false;
        std::int64_t retry_backoff_ms = 0;
        {
            std::lock_guard lock(state->mutex);
            apply_replenish_result_locked(state, std::move(replacement), log_retry_with_backoff, retry_backoff_ms);
        }

        if (log_retry_with_backoff) {
            Logger::instance()
                .warn(LogDomain::Common, "postgres connection replenish failed")
                .field("error", "connection_create_failed")
                .field("timeout_ms", retry_backoff_ms)
                .field("decision", "retry_with_backoff");
        }
        state->condition.notify_all();
    }
}

bool PostgresConnectionPool::ensure_replenish_worker_locked(const std::shared_ptr<PoolState>& state) {
    if (!state) {
        return false;
    }
    if (state->replenish_worker.joinable()) {
        return true;
    }
    if (state->fail_replenish_worker_start_for_test) {
        Logger::instance()
            .error(LogDomain::Common, "postgres connection replenish worker start failed")
            .field("error", "forced_for_test")
            .field("decision", "keep_waiting");
        return false;
    }

    try {
        state->replenish_worker = std::jthread([state]() { replenish_worker_loop(state); });
        return true;
    } catch (const std::exception& e) {
        Logger::instance()
            .error(LogDomain::Common, "postgres connection replenish worker start failed")
            .field("error", e.what())
            .field("decision", "keep_waiting");
    } catch (...) {
        Logger::instance()
            .error(LogDomain::Common, "postgres connection replenish worker start failed")
            .field("error", "unknown")
            .field("decision", "keep_waiting");
    }
    return false;
}

bool PostgresConnectionPool::schedule_replenish_connection_locked(const std::shared_ptr<PoolState>& state) {
    if (!state || !state->initialized || state->shutting_down) {
        return false;
    }
    if (!ensure_replenish_worker_locked(state)) {
        return false;
    }

    const std::size_t connection_count = state->available_connections.size() + state->in_use_connections;
    if (connection_count >= state->options.max_connections) {
        state->replenish_requested = false;
        return false;
    }

    if (!state->replenish_requested) {
        state->replenish_requested = true;
        state->next_replenish_retry_at = std::chrono::steady_clock::now();
    }
    state->condition.notify_all();
    return true;
}

void PostgresConnectionPool::release_connection_to_state(const std::shared_ptr<PoolState>& state,
                                                         std::unique_ptr<pqxx::connection> connection) {
    if (!state) {
        return;
    }

    bool discarded_unhealthy_connection = false;
    if (connection && !connection->is_open()) {
        Logger::instance()
            .warn(LogDomain::Common, "postgres connection unhealthy")
            .field("error", "connection_not_open")
            .field("decision", "discard_connection");
        connection.reset();
        discarded_unhealthy_connection = true;
    }

    {
        std::lock_guard lock(state->mutex);
        if (state->in_use_connections > 0U) {
            --state->in_use_connections;
        }
        if (!state->shutting_down && connection) {
            state->available_connections.push_back(std::move(connection));
        } else if (discarded_unhealthy_connection) {
            schedule_replenish_connection_locked(state);
        }
    }
    state->condition.notify_one();
}

}  // namespace nebula::common
