#ifndef NEBULA_STORAGE_REPOSITORY_SCHEMA_ROW_PARSER_HPP
#define NEBULA_STORAGE_REPOSITORY_SCHEMA_ROW_PARSER_HPP

#include <cstddef>
#include <pqxx/pqxx>

#include "nebula/database/row_check.hpp"

namespace nebula::storage {

inline constexpr std::size_t kStorageRepositorySchemaReadyFieldCount = 10;

database::RowCheckStatus parse_storage_repository_schema_ready_row(const pqxx::row& row);

}  // namespace nebula::storage

#endif  // NEBULA_STORAGE_REPOSITORY_SCHEMA_ROW_PARSER_HPP
