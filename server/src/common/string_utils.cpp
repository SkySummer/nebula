#include "nebula/common/string_utils.hpp"

#include <cctype>

namespace nebula::common {

std::string_view trim_ascii(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }

    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1U])) != 0) {
        --end;
    }

    return text.substr(begin, end - begin);
}

}  // namespace nebula::common
