#ifndef NEBULA_TESTS_COMMON_HPP
#define NEBULA_TESTS_COMMON_HPP

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "nebula/common/codec/base64.hpp"
#include "nebula/common/platform/file_io.hpp"
#include "nebula/common/platform/posix.hpp"

namespace nebula::test {

class TempDir {
public:
    explicit TempDir(std::string_view prefix) {
        static std::atomic<uint64_t> counter = 0;
        const uint64_t id = counter.fetch_add(1);
        path_ = std::filesystem::temp_directory_path() / std::format("{}-{}-{}", prefix, ::getpid(), id);

        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_, ec);
        if (ec) {
            throw std::runtime_error(
                std::format("failed to create temp dir '{}': {}", path_.generic_string(), ec.message()));
        }
    }

    ~TempDir() noexcept {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = default;
    TempDir& operator=(TempDir&&) = default;

    [[nodiscard]] const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

class ArgvBuilder {
public:
    explicit ArgvBuilder(std::vector<std::string> args) : storage_(std::move(args)) {
        argv_.reserve(storage_.size());
        for (std::string& item : storage_) {
            argv_.push_back(item.data());
        }
    }

    [[nodiscard]] std::span<char*> span() {
        return {argv_.data(), argv_.size()};
    }

private:
    std::vector<std::string> storage_;
    std::vector<char*> argv_;
};

class ScopedEnvVar {
public:
    ScopedEnvVar(std::string name, std::string_view value) : name_(std::move(name)) {
        const char* existing = ::getenv(name_.c_str());
        if (existing != nullptr) {
            had_value_ = true;
            original_value_ = existing;
        }

        ::setenv(name_.c_str(), std::string(value).c_str(), 1);
    }

    ~ScopedEnvVar() noexcept {
        if (had_value_) {
            ::setenv(name_.c_str(), original_value_.c_str(), 1);
            return;
        }

        ::unsetenv(name_.c_str());
    }

    ScopedEnvVar(const ScopedEnvVar&) = delete;
    ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;
    ScopedEnvVar(ScopedEnvVar&&) = delete;
    ScopedEnvVar& operator=(ScopedEnvVar&&) = delete;

private:
    std::string name_;
    bool had_value_ = false;
    std::string original_value_;
};

[[noreturn]] inline void fail(std::string_view message) {
    throw std::runtime_error(std::string(message));
}

template <typename T>
inline std::string to_debug_string(const T& value) {
    if constexpr (requires(std::ostringstream& stream, const T& v) { stream << v; }) {
        std::ostringstream stream;
        stream << value;
        return stream.str();
    } else {
        return "<unprintable>";
    }
}

inline void expect_true(bool condition, std::string_view message) {
    if (!condition) {
        fail(message);
    }
}

template <typename T, typename U>
inline void expect_equal(const T& actual, const U& expected, std::string_view message) {
    if (!(actual == expected)) {
        throw std::runtime_error(
            std::format("{}: actual='{}', expected='{}'", message, to_debug_string(actual), to_debug_string(expected)));
    }
}

inline void expect_contains(std::string_view text, std::string_view needle, std::string_view message) {
    if (text.find(needle) == std::string_view::npos) {
        throw std::runtime_error(std::format("{}: missing '{}'", message, needle));
    }
}

inline void expect_not_contains(std::string_view text, std::string_view needle, std::string_view message) {
    if (text.find(needle) != std::string_view::npos) {
        throw std::runtime_error(std::format("{}: found unexpected '{}'", message, needle));
    }
}

inline std::size_t count_occurrences(std::string_view text, std::string_view needle) {
    if (needle.empty()) {
        return 0;
    }

    std::size_t count = 0;
    std::size_t pos = 0;
    while (true) {
        pos = text.find(needle, pos);
        if (pos == std::string_view::npos) {
            return count;
        }
        ++count;
        pos += needle.size();
    }
}

inline std::filesystem::path find_single_regular_file(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file()) {
            files.push_back(entry.path());
        }
    }

    if (files.size() != 1U) {
        throw std::runtime_error(
            std::format("expected exactly one file in '{}', actual={}", dir.generic_string(), files.size()));
    }
    return files.front();
}

inline std::string read_all(const std::filesystem::path& file_path) {
    auto content = nebula::common::read_file(file_path);
    if (!content.has_value()) {
        fail(std::format("failed to read file: error={}", nebula::common::to_string(content.error())));
    }
    return std::move(*content);
}

inline void write_file(const std::filesystem::path& path, std::string_view content) {
    const auto write_result = nebula::common::write_file(path, content);
    if (!write_result.has_value()) {
        fail(
            std::format("config file write should succeed: error={}", nebula::common::to_string(write_result.error())));
    }
}

inline void write_binary_file(const std::filesystem::path& path, std::string_view content) {
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }
    const auto bytes = std::as_bytes(std::span(content.data(), content.size()));
    const auto write_result = nebula::common::write_binary_file(path, bytes);
    if (!write_result.has_value()) {
        fail(std::format("test file write should succeed: error={}", nebula::common::to_string(write_result.error())));
    }
}

inline std::vector<std::byte> to_byte_vector(std::string_view value) {
    std::vector<std::byte> out;
    out.reserve(value.size());
    for (const char ch : value) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    }
    return out;
}

inline void set_owner_read_write_only(const std::filesystem::path& path) {
    std::error_code permissions_error;
    std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, permissions_error);
    expect_true(!permissions_error, "file permissions should be set to owner_read_write");
}

inline void set_owner_read_only(const std::filesystem::path& path) {
    std::error_code permissions_error;
    std::filesystem::permissions(path, std::filesystem::perms::owner_read, std::filesystem::perm_options::replace,
                                 permissions_error);
    expect_true(!permissions_error, "file permissions should be set to owner_read");
}

inline void write_jwt_secret_file(const std::filesystem::path& path, std::string_view secret) {
    write_file(path, nebula::common::base64_encode(secret));
    set_owner_read_write_only(path);
}

inline void write_jwt_secret_file(const std::filesystem::path& path, std::span<const std::byte> secret) {
    write_file(path, nebula::common::base64_encode(secret));
    set_owner_read_write_only(path);
}

template <typename Fn>
inline std::string capture_stderr(Fn&& fn, std::string_view prefix = "nebula-stderr-capture") {
    static std::atomic<uint64_t> counter = 0;
    const uint64_t id = counter.fetch_add(1);
    const std::filesystem::path capture_path =
        std::filesystem::temp_directory_path() / std::format("{}-{}-{}.log", prefix, ::getpid(), id);

    const int capture_fd = ::creat(capture_path.c_str(), 0600);
    if (capture_fd < 0) {
        const int err = errno;
        throw std::runtime_error(std::format("failed to open stderr capture file '{}': errno={} ({})",
                                             capture_path.generic_string(), err, nebula::common::errno_message(err)));
    }

    const int saved_stderr_fd = ::dup(STDERR_FILENO);
    if (saved_stderr_fd < 0) {
        const int err = errno;
        ::close(capture_fd);
        throw std::runtime_error(
            std::format("failed to dup stderr: errno={} ({})", err, nebula::common::errno_message(err)));
    }

    if (::dup2(capture_fd, STDERR_FILENO) < 0) {
        const int err = errno;
        ::close(capture_fd);
        ::close(saved_stderr_fd);
        throw std::runtime_error(
            std::format("failed to redirect stderr: errno={} ({})", err, nebula::common::errno_message(err)));
    }
    ::close(capture_fd);

    try {
        std::invoke(std::forward<Fn>(fn));
    } catch (...) {
        std::fflush(stderr);
        ::dup2(saved_stderr_fd, STDERR_FILENO);
        ::close(saved_stderr_fd);
        std::error_code ec;
        std::filesystem::remove(capture_path, ec);
        throw;
    }

    std::fflush(stderr);
    if (::dup2(saved_stderr_fd, STDERR_FILENO) < 0) {
        const int err = errno;
        ::close(saved_stderr_fd);
        throw std::runtime_error(
            std::format("failed to restore stderr: errno={} ({})", err, nebula::common::errno_message(err)));
    }
    ::close(saved_stderr_fd);

    const std::string content = read_all(capture_path);
    std::error_code ec;
    std::filesystem::remove(capture_path, ec);
    return content;
}

template <typename Fn>
inline int run_main(Fn&& fn) noexcept {
    try {
        return std::invoke(std::forward<Fn>(fn));
    } catch (const std::exception& e) {
        std::cerr << "unhandled exception in main: error=" << e.what() << '\n';
    } catch (...) {
        std::cerr << "unhandled exception in main: error=unknown\n";
    }
    return 1;
}

using TestCase = std::pair<std::string_view, std::function<void()>>;

inline int run_tests(const std::vector<TestCase>& tests) {
    std::size_t passed = 0;
    std::size_t failed = 0;

    for (const auto& [name, test] : tests) {
        try {
            test();
            ++passed;
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& e) {
            ++failed;
            std::cerr << "[FAIL] " << name << ": " << e.what() << '\n';
        } catch (...) {
            ++failed;
            std::cerr << "[FAIL] " << name << ": unknown exception\n";
        }
    }

    std::cout << "Summary: passed=" << passed << ", failed=" << failed << ", total=" << tests.size() << '\n';
    return failed == 0U ? 0 : 1;
}

}  // namespace nebula::test

#endif  // NEBULA_TESTS_COMMON_HPP
