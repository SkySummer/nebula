#include "nebula/http/routing/path.hpp"

#include <algorithm>

namespace nebula::http {

std::vector<std::string> split_http_path_segments(std::string_view path) {
    std::vector<std::string> segments;
    segments.reserve(static_cast<std::size_t>(std::count(path.begin(), path.end(), '/')) + 1U);

    std::size_t cursor = 0;
    while (true) {
        const std::size_t slash_pos = path.find('/', cursor);
        if (slash_pos == std::string_view::npos) {
            segments.emplace_back(path.substr(cursor));
            break;
        }

        segments.emplace_back(path.substr(cursor, slash_pos - cursor));
        cursor = slash_pos + 1U;
    }

    return segments;
}

}  // namespace nebula::http
