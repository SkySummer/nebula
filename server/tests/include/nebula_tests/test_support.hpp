#ifndef NEBULA_TESTS_TEST_SUPPORT_HPP
#define NEBULA_TESTS_TEST_SUPPORT_HPP

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace nebula::testsupport {

[[noreturn]] inline void fail(std::string_view message) {
    throw std::runtime_error(std::string(message));
}

inline void expect_true(bool condition, std::string_view message) {
    if (!condition) {
        fail(message);
    }
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

class TempDir {
public:
    explicit TempDir(std::string_view prefix) {
        static std::atomic<uint64_t> counter = 0;
        const uint64_t id = counter.fetch_add(1);
        path_ = std::filesystem::temp_directory_path() / std::format("{}-{}-{}", prefix, getpid(), id);

        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_, ec);
        if (ec) {
            throw std::runtime_error(std::format("failed to create temp dir '{}': {}", path_.string(), ec.message()));
        }
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = default;
    TempDir& operator=(TempDir&&) = default;

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    [[nodiscard]] const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

inline std::filesystem::path find_single_regular_file(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file()) {
            files.push_back(entry.path());
        }
    }

    if (files.size() != 1U) {
        throw std::runtime_error(
            std::format("expected exactly one file in '{}', actual={}", dir.string(), files.size()));
    }
    return files.front();
}

inline std::string read_all(const std::filesystem::path& file_path) {
    std::ifstream stream(file_path);
    expect_true(stream.is_open(), "failed to open file");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

inline void write_file(const std::filesystem::path& path, std::string_view content) {
    std::ofstream stream(path);
    expect_true(stream.is_open(), "config file should open for write");
    stream << content;
    stream.flush();
    expect_true(stream.good(), "config file should flush successfully");
}

class ArgvBuilder {
public:
    explicit ArgvBuilder(std::vector<std::string> args) : storage_(std::move(args)) {
        rebuild();
    }

    [[nodiscard]] std::span<char*> span() {
        return {argv_.data(), argv_.size()};
    }

private:
    void rebuild() {
        argv_.clear();
        argv_.reserve(storage_.size());
        for (std::string& item : storage_) {
            argv_.push_back(item.data());
        }
    }

    std::vector<std::string> storage_;
    std::vector<char*> argv_;
};

template <typename Fn>
inline std::string capture_stderr(Fn&& fn, std::string_view prefix = "nebula-stderr-capture") {
    static std::atomic<uint64_t> counter = 0;
    const uint64_t id = counter.fetch_add(1);
    const std::filesystem::path capture_path =
        std::filesystem::temp_directory_path() / std::format("{}-{}-{}.log", prefix, getpid(), id);

    const int capture_fd = ::creat(capture_path.c_str(), 0600);
    if (capture_fd < 0) {
        throw std::runtime_error(std::format("failed to open stderr capture file '{}': errno={} ({})",
                                             capture_path.string(), errno, std::system_category().message(errno)));
    }

    const int saved_stderr_fd = ::dup(STDERR_FILENO);
    if (saved_stderr_fd < 0) {
        const int err = errno;
        ::close(capture_fd);
        throw std::runtime_error(
            std::format("failed to dup stderr: errno={} ({})", err, std::system_category().message(err)));
    }

    if (::dup2(capture_fd, STDERR_FILENO) < 0) {
        const int err = errno;
        ::close(capture_fd);
        ::close(saved_stderr_fd);
        throw std::runtime_error(
            std::format("failed to redirect stderr: errno={} ({})", err, std::system_category().message(err)));
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
            std::format("failed to restore stderr: errno={} ({})", err, std::system_category().message(err)));
    }
    ::close(saved_stderr_fd);

    const std::string content = read_all(capture_path);
    std::error_code ec;
    std::filesystem::remove(capture_path, ec);
    return content;
}

using TestCase = std::pair<std::string_view, std::function<void()>>;

template <typename Fn>
inline int run_main(Fn&& fn) noexcept {
    try {
        return std::invoke(std::forward<Fn>(fn));
    } catch (const std::exception& ex) {
        std::fputs("unhandled exception in main: error=", stderr);
        std::fputs(ex.what(), stderr);
        std::fputc('\n', stderr);
    } catch (...) {
        std::fputs("unhandled exception in main: error=unknown\n", stderr);
    }
    return 1;
}

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

}  // namespace nebula::testsupport

#endif  // NEBULA_TESTS_TEST_SUPPORT_HPP
