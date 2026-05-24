#include "nebula/http/redaction/request_redaction.hpp"

#include <array>

namespace nebula::http {

namespace {

struct LogRedactionRule {
    std::string_view path_prefix;
    std::string_view replacement;
};

constexpr std::array<LogRedactionRule, 1> kLogRedactionRules{{
    {.path_prefix = "/api/storage/downloads/", .replacement = "/api/storage/downloads/{download_ticket}"},
}};

std::size_t find_request_target_suffix(std::string_view request_target) {
    return request_target.find_first_of("?#");
}

std::string redact_request_target(std::string_view request_target) {
    if (request_target.empty()) {
        return {};
    }

    if (request_target.front() == '/') {
        const std::size_t suffix_begin = find_request_target_suffix(request_target);
        const std::string_view path = request_target.substr(0, suffix_begin);
        std::string sanitized = redact_request_path(path);
        if (sanitized == path) {
            return std::string(request_target);
        }
        if (suffix_begin != std::string_view::npos) {
            sanitized.append(request_target.substr(suffix_begin));
        }
        return sanitized;
    }

    const std::size_t scheme_pos = request_target.find("://");
    if (scheme_pos == std::string_view::npos) {
        return std::string(request_target);
    }

    const std::size_t path_begin = request_target.find('/', scheme_pos + 3U);
    if (path_begin == std::string_view::npos) {
        return std::string(request_target);
    }

    const std::size_t suffix_begin = find_request_target_suffix(request_target.substr(path_begin));
    const std::size_t suffix_offset =
        suffix_begin == std::string_view::npos ? std::string_view::npos : path_begin + suffix_begin;
    const std::string_view path = request_target.substr(
        path_begin, suffix_offset == std::string_view::npos ? std::string_view::npos : suffix_offset - path_begin);
    std::string sanitized_path = redact_request_path(path);
    if (sanitized_path == path) {
        return std::string(request_target);
    }

    std::string sanitized(request_target.substr(0, path_begin));
    sanitized.append(sanitized_path);
    if (suffix_offset != std::string_view::npos) {
        sanitized.append(request_target.substr(suffix_offset));
    }
    return sanitized;
}

}  // namespace

std::string redact_request_path(std::string_view path) {
    for (const LogRedactionRule& rule : kLogRedactionRules) {
        if (!path.starts_with(rule.path_prefix)) {
            continue;
        }

        const std::string_view suffix = path.substr(rule.path_prefix.size());
        if (suffix.empty()) {
            return std::string(path);
        }

        const std::size_t suffix_separator = suffix.find_first_of("/?#");
        if (suffix_separator == 0U) {
            return std::string(path);
        }

        std::string sanitized(rule.replacement);
        if (suffix_separator != std::string_view::npos) {
            sanitized.append(suffix.substr(suffix_separator));
        }
        return sanitized;
    }
    return std::string(path);
}

std::string redact_request_line(std::string_view request_line) {
    const std::size_t method_end = request_line.find(' ');
    if (method_end == std::string_view::npos) {
        return std::string(request_line);
    }

    const std::size_t version_begin = request_line.rfind(' ');
    if (version_begin == std::string_view::npos || version_begin <= method_end) {
        return std::string(request_line);
    }

    const std::string_view request_target = request_line.substr(method_end + 1U, version_begin - method_end - 1U);
    const std::string sanitized_target = redact_request_target(request_target);
    if (sanitized_target == request_target) {
        return std::string(request_line);
    }

    std::string sanitized(request_line.substr(0, method_end + 1U));
    sanitized.append(sanitized_target);
    sanitized.push_back(' ');
    sanitized.append(request_line.substr(version_begin + 1U));
    return sanitized;
}

}  // namespace nebula::http
