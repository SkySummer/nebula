#include "nebula/common/base/string.hpp"

#include <cctype>
#include <string_view>
#include <utility>
#include <vector>

namespace nebula::common {

std::string_view to_string(ParseNumberError error) noexcept {
    switch (error) {
        case ParseNumberError::Empty:
            return "empty";
        case ParseNumberError::Invalid:
            return "invalid";
        case ParseNumberError::OutOfRange:
            return "out_of_range";
    }
    std::unreachable();
}

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

std::vector<std::string_view> split(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t pos = text.find(separator, start);
        if (pos == std::string_view::npos) {
            parts.push_back(text.substr(start));
            break;
        }
        parts.push_back(text.substr(start, pos - start));
        start = pos + 1U;
    }
    return parts;
}

}  // namespace nebula::common
