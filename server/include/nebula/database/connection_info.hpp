#ifndef NEBULA_DATABASE_CONNECTION_INFO_HPP
#define NEBULA_DATABASE_CONNECTION_INFO_HPP

#include <string>

#include "nebula/database/config.hpp"

namespace nebula::database {

[[nodiscard]] std::string build_connection_info(const DatabaseConfig& config);

}  // namespace nebula::database

#endif  // NEBULA_DATABASE_CONNECTION_INFO_HPP
