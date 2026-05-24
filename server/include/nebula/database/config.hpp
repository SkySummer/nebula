#ifndef NEBULA_DATABASE_CONFIG_HPP
#define NEBULA_DATABASE_CONFIG_HPP

#include <cstddef>
#include <cstdint>
#include <string>

namespace nebula::database {

inline constexpr std::size_t kMaxDatabaseConnections = 1024U;
inline constexpr std::int64_t kMaxDatabaseConnectTimeoutS = 60;
inline constexpr std::int64_t kMaxDatabaseAcquireTimeoutMs = 60'000;

struct DatabaseConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 5432;
    std::string name = "nebula";
    std::string user = "nebula";
    std::string password_env = "NEBULA_DATABASE_PASSWORD";
    std::string password;
    std::size_t max_connections = 8;
    std::int64_t connect_timeout_s = 3;
    std::int64_t acquire_timeout_ms = 3000;

    DatabaseConfig& normalize() &;
    DatabaseConfig&& normalize() &&;

    [[nodiscard]] bool validate() const;
};

}  // namespace nebula::database

#endif  // NEBULA_DATABASE_CONFIG_HPP
