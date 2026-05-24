#ifndef NEBULA_STORAGE_DOMAIN_FILE_TYPES_HPP
#define NEBULA_STORAGE_DOMAIN_FILE_TYPES_HPP

#include <string_view>

namespace nebula::storage {

[[nodiscard]] std::string_view classify_file_type(std::string_view path);

}  // namespace nebula::storage

#endif  // NEBULA_STORAGE_DOMAIN_FILE_TYPES_HPP
