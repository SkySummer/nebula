#include "nebula/database/row_check.hpp"

#include <cstdlib>
#include <pqxx/pqxx>
#include <utility>

namespace nebula::database {

RowCheckStatus check_row_ready(const pqxx::row& row, std::size_t expected_size) {
    if (std::cmp_not_equal(row.size(), expected_size)) {
        return RowCheckStatus::InvalidSize;
    }

    for (const pqxx::field& field : row) {
        if (field.is_null()) {
            return RowCheckStatus::NullField;
        }
    }
    return RowCheckStatus::Ready;
}

RowCheckStatus check_row_required_fields(const pqxx::row& row, std::size_t expected_size,
                                         std::initializer_list<std::size_t> required_fields) {
    if (std::cmp_not_equal(row.size(), expected_size)) {
        return RowCheckStatus::InvalidSize;
    }

    for (const std::size_t index : required_fields) {
        if (index >= expected_size) {
            return RowCheckStatus::NullField;
        }

        const auto row_index = static_cast<pqxx::row::size_type>(index);
        if (row[row_index].is_null()) {
            return RowCheckStatus::NullField;
        }
    }
    return RowCheckStatus::Ready;
}

}  // namespace nebula::database
