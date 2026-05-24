#include "nebula/storage/repository/schema_row_parser.hpp"

namespace nebula::storage {

database::RowCheckStatus parse_storage_repository_schema_ready_row(const pqxx::row& row) {
    return database::check_row_ready(row, kStorageRepositorySchemaReadyFieldCount);
}

}  // namespace nebula::storage
