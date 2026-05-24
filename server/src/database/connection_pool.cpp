#include "nebula/database/connection_pool.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <utility>

#include "nebula/common/log/logger.hpp"
#include "nebula/database/connection_info.hpp"

namespace nebula::database {

namespace {

constexpr auto kDefaultReplenishRetryBackoff = std::chrono::milliseconds{100};
constexpr auto kMaxReplenishRetryBackoff = std::chrono::milliseconds{1600};

std::unique_ptr<pqxx::connection> create_connection(const DatabaseConfig& config) noexcept {
    try {
        auto connection = std::make_unique<pqxx::connection>(build_connection_info(config));
        if (!connection->is_open()) {
            return nullptr;
        }
        return connection;
    } catch (...) {
        return nullptr;
    }
}

}  // namespace

ConnectionLease::ConnectionLease(std::shared_ptr<ConnectionPool> pool, std::unique_ptr<pqxx::connection> connection)
    : pool_(std::move(pool)), connection_(std::move(connection)) {}

ConnectionLease::~ConnectionLease() noexcept {
    if (!pool_ || !connection_) {
        return;
    }

    try {
        pool_->release_connection(std::move(connection_));
    } catch (const std::exception& e) {
        common::Logger::instance()
            .error("postgres connection lease destructor release failed")
            .field("error", e.what())
            .field("decision", "ignore");
    } catch (...) {
        common::Logger::instance()
            .error("postgres connection lease destructor release failed")
            .field("error", "unknown")
            .field("decision", "ignore");
    }
}

pqxx::connection& ConnectionLease::connection() const {
    return *connection_;
}

ConnectionPool::ConnectionPool(DatabaseConfig config)
    : config_(std::move(config)),
      next_replenish_retry_at_(std::chrono::steady_clock::now()),
      replenish_retry_backoff_(kDefaultReplenishRetryBackoff) {
    available_connections_.reserve(config_.max_connections);
    for (std::size_t idx = 0; idx < config_.max_connections; ++idx) {
        auto connection = create_connection(config_);
        if (!connection) {
            available_connections_.clear();
            throw std::runtime_error("connection_create_failed");
        }
        available_connections_.push_back(std::move(connection));
    }

    try {
        replenish_worker_ = std::jthread([this](const std::stop_token& stop) { replenish_worker_loop(stop); });
    } catch (...) {
        available_connections_.clear();
        replenish_in_progress_ = false;
        replenish_requested_ = false;
        throw std::runtime_error("replenish_worker_start_failed");
    }

    common::Logger::instance()
        .info("postgres connection pool created")
        .field("count", available_connections_.size())
        .field("max_connections", config_.max_connections);
}

ConnectionPool::~ConnectionPool() noexcept {
    try {
        {
            std::lock_guard lock(mutex_);
            state_ = PoolState::Stopping;
            replenish_in_progress_ = false;
            replenish_requested_ = false;
            available_connections_.clear();
        }
        replenish_worker_.request_stop();
        condition_.notify_all();
    } catch (const std::exception& e) {
        common::Logger::instance()
            .error("postgres connection pool destructor cleanup failed")
            .field("error", e.what())
            .field("decision", "ignore");
    } catch (...) {
        common::Logger::instance()
            .error("postgres connection pool destructor cleanup failed")
            .field("error", "unknown")
            .field("decision", "ignore");
    }
}

std::shared_ptr<ConnectionPool> ConnectionPool::create(const DatabaseConfig& config) {
    if (!config.validate()) {
        common::Logger::instance()
            .error("postgres connection pool create failed")
            .field("error", "invalid_database_config")
            .field("decision", "return_nullptr");
        return nullptr;
    }
    if (config.password.empty()) {
        common::Logger::instance()
            .error("postgres connection pool create failed")
            .field("error", "missing_database_password")
            .field("decision", "return_nullptr");
        return nullptr;
    }

    try {
        return std::shared_ptr<ConnectionPool>(new ConnectionPool(config));
    } catch (const std::invalid_argument& e) {
        common::Logger::instance()
            .error("postgres connection pool create failed")
            .field("error", e.what())
            .field("decision", "return_nullptr");
        return nullptr;
    } catch (const std::runtime_error& e) {
        common::Logger::instance()
            .error("postgres connection pool create failed")
            .field("error", e.what())
            .field("decision", "return_nullptr");
        return nullptr;
    } catch (const std::exception& e) {
        common::Logger::instance()
            .error("postgres connection pool create failed")
            .field("error", e.what())
            .field("decision", "return_nullptr");
        return nullptr;
    } catch (...) {
        common::Logger::instance()
            .error("postgres connection pool create failed")
            .field("error", "unknown")
            .field("decision", "return_nullptr");
        return nullptr;
    }
}

std::optional<ConnectionLease> ConnectionPool::acquire_lease(const std::source_location location) {
    const std::shared_ptr<ConnectionPool> pool = shared_from_this();
    std::unique_lock lock(mutex_);

    const auto acquire_timeout = std::chrono::milliseconds{config_.acquire_timeout_ms};
    const std::size_t max_connections = config_.max_connections;
    const auto deadline = std::chrono::steady_clock::now() + acquire_timeout;

    while (true) {
        if (state_ == PoolState::Stopping) {
            common::Logger::instance()
                .warn("postgres connection lease acquire failed")
                .field("function", location.function_name())
                .field("error", "pool_stopping")
                .field("decision", "return_nullopt");
            return std::nullopt;
        }

        if (!available_connections_.empty()) {
            auto connection = std::move(available_connections_.back());
            available_connections_.pop_back();
            ++in_use_connections_;

            if (connection && connection->is_open()) {
                return ConnectionLease(pool, std::move(connection));
            }

            --in_use_connections_;
            schedule_replenish_connection_locked();
            common::Logger::instance()
                .warn("postgres connection unhealthy")
                .field("error", "connection_not_open")
                .field("decision", "discard_connection");
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            common::Logger::instance()
                .warn("postgres connection lease acquire timed out")
                .field("function", location.function_name())
                .field("timeout_ms", acquire_timeout.count())
                .field("max_connections", max_connections)
                .field("in_use_connections", in_use_connections_)
                .field("available_connections", available_connections_.size())
                .field("decision", "return_timed_out");
            return std::nullopt;
        }

        schedule_replenish_connection_locked();
        condition_.wait_until(lock, deadline);
    }
}

void ConnectionPool::release_connection(std::unique_ptr<pqxx::connection> connection) {
    bool discarded_unhealthy_connection = false;
    if (connection && !connection->is_open()) {
        common::Logger::instance()
            .warn("postgres connection unhealthy")
            .field("error", "connection_not_open")
            .field("decision", "discard_connection");
        connection.reset();
        discarded_unhealthy_connection = true;
    }

    bool notify_replenish_worker = false;
    {
        std::lock_guard lock(mutex_);
        if (in_use_connections_ == 0U) {
            common::Logger::instance()
                .fatal("postgres connection pool invariant violated")
                .field("error", "in_use_connections_underflow")
                .field("in_use_connections", in_use_connections_)
                .field("available_connections", available_connections_.size())
                .field("max_connections", config_.max_connections)
                .field("decision", "terminate");
            std::terminate();
        }
        --in_use_connections_;

        if (state_ != PoolState::Stopping && connection) {
            available_connections_.push_back(std::move(connection));
        } else if (discarded_unhealthy_connection) {
            const std::size_t connection_count = connection_count_locked();
            if (state_ != PoolState::Stopping && connection_count < config_.max_connections) {
                request_replenish_locked();
                notify_replenish_worker = true;
            }
        }
    }
    if (notify_replenish_worker) {
        condition_.notify_all();
    }
    condition_.notify_one();
}

std::size_t ConnectionPool::connection_count_locked() const noexcept {
    return available_connections_.size() + in_use_connections_;
}

bool ConnectionPool::prepare_replenish_attempt_locked() {
    if (state_ == PoolState::Stopping) {
        return false;
    }
    if (!replenish_requested_ || replenish_in_progress_) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now < next_replenish_retry_at_) {
        return false;
    }

    const std::size_t connection_count = connection_count_locked();
    if (connection_count >= config_.max_connections) {
        replenish_requested_ = false;
        replenish_retry_backoff_ = kDefaultReplenishRetryBackoff;
        return false;
    }

    replenish_in_progress_ = true;
    return true;
}

void ConnectionPool::request_replenish_locked() {
    if (replenish_requested_) {
        return;
    }

    replenish_requested_ = true;
    next_replenish_retry_at_ = std::chrono::steady_clock::now();
}

void ConnectionPool::apply_replenish_result(std::unique_ptr<pqxx::connection> replacement, bool& log_retry_with_backoff,
                                            std::chrono::milliseconds& retry_backoff) {
    std::lock_guard lock(mutex_);
    replenish_in_progress_ = false;
    const std::size_t current_count = connection_count_locked();
    if (state_ == PoolState::Stopping || current_count >= config_.max_connections) {
        replenish_requested_ = false;
        replenish_retry_backoff_ = kDefaultReplenishRetryBackoff;
        return;
    }

    if (replacement) {
        available_connections_.push_back(std::move(replacement));
        const std::size_t replenished_count = connection_count_locked();
        replenish_requested_ = replenished_count < config_.max_connections;
        next_replenish_retry_at_ = std::chrono::steady_clock::now();
        replenish_retry_backoff_ = kDefaultReplenishRetryBackoff;
        return;
    }

    log_retry_with_backoff = true;
    retry_backoff = replenish_retry_backoff_;
    replenish_requested_ = true;
    next_replenish_retry_at_ = std::chrono::steady_clock::now() + retry_backoff;
    replenish_retry_backoff_ = std::min(kMaxReplenishRetryBackoff, retry_backoff * 2);
}

void ConnectionPool::replenish_worker_loop(const std::stop_token& stop) {
    while (true) {
        {
            std::unique_lock lock(mutex_);
            while (true) {
                if (prepare_replenish_attempt_locked()) {
                    break;
                }
                if (state_ == PoolState::Stopping || stop.stop_requested()) {
                    return;
                }

                const bool wait_for_retry_backoff = replenish_requested_ && !replenish_in_progress_ &&
                                                    std::chrono::steady_clock::now() < next_replenish_retry_at_;
                if (wait_for_retry_backoff) {
                    condition_.wait_until(lock, stop, next_replenish_retry_at_, [this]() {
                        return prepare_replenish_attempt_locked() || state_ == PoolState::Stopping;
                    });
                } else {
                    condition_.wait(lock, stop, [this]() {
                        return prepare_replenish_attempt_locked() || state_ == PoolState::Stopping;
                    });
                }

                if (replenish_in_progress_) {
                    break;
                }
            }
        }

        auto replacement = create_connection(config_);

        bool log_retry_with_backoff = false;
        std::chrono::milliseconds retry_backoff{0};
        apply_replenish_result(std::move(replacement), log_retry_with_backoff, retry_backoff);

        if (log_retry_with_backoff) {
            common::Logger::instance()
                .warn("postgres connection replenish failed")
                .field("error", "connection_create_failed")
                .field("timeout_ms", retry_backoff.count())
                .field("decision", "retry_with_backoff");
        }
        condition_.notify_all();
    }
}

bool ConnectionPool::schedule_replenish_connection_locked() {
    if (state_ == PoolState::Stopping) {
        return false;
    }

    const std::size_t connection_count = connection_count_locked();
    if (connection_count >= config_.max_connections) {
        replenish_requested_ = false;
        return false;
    }

    request_replenish_locked();
    condition_.notify_all();
    return true;
}

}  // namespace nebula::database
