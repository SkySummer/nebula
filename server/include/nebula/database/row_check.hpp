#ifndef NEBULA_DATABASE_ROW_CHECK_HPP
#define NEBULA_DATABASE_ROW_CHECK_HPP

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <pqxx/pqxx>

namespace nebula::database {

enum class RowCheckStatus : std::uint8_t {
    Ready,
    InvalidSize,
    NullField,
};

[[nodiscard]] RowCheckStatus check_row_ready(const pqxx::row& row, std::size_t expected_size);

[[nodiscard]] RowCheckStatus check_row_required_fields(const pqxx::row& row, std::size_t expected_size,
                                                       std::initializer_list<std::size_t> required_fields);

}  // namespace nebula::database

#endif  // NEBULA_DATABASE_ROW_CHECK_HPP
