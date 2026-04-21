#include "nebula/common/logger.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "nebula_tests/test_support.hpp"

namespace {

using nebula::common::LogDomain;
using nebula::common::Logger;
using nebula::common::LogLevel;
using nebula::testsupport::capture_stderr;
using nebula::testsupport::expect_contains;
using nebula::testsupport::expect_equal;
using nebula::testsupport::expect_not_contains;
using nebula::testsupport::expect_true;
using nebula::testsupport::find_single_regular_file;
using nebula::testsupport::read_all;
using nebula::testsupport::TempDir;

std::string first_line(std::string_view text) {
    const std::size_t pos = text.find('\n');
    if (pos == std::string_view::npos) {
        return std::string(text);
    }
    return std::string(text.substr(0, pos));
}

std::string logger_current_date_text() {
    const auto now = std::chrono::system_clock::now();
    try {
        const std::chrono::time_zone* zone = std::chrono::current_zone();
        const std::chrono::zoned_time zoned_now{zone, now};
        return std::format("{:%F}", std::chrono::floor<std::chrono::milliseconds>(zoned_now.get_local_time()));
    } catch (const std::runtime_error&) {
        return std::format("{:%F}", std::chrono::floor<std::chrono::milliseconds>(now));
    }
}

std::size_t count_regular_files(const std::filesystem::path& dir) {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file()) {
            ++count;
        }
    }
    return count;
}

std::size_t count_occurrences(std::string_view text, std::string_view needle) {
    if (needle.empty()) {
        return 0U;
    }

    std::size_t count = 0U;
    std::size_t pos = 0U;
    while (true) {
        pos = text.find(needle, pos);
        if (pos == std::string_view::npos) {
            return count;
        }
        ++count;
        pos += needle.size();
    }
}

void test_to_string_covers_levels() {
    expect_equal(std::string(nebula::common::to_string(LogLevel::Trace)), std::string("TRACE"), "trace text");
    expect_equal(std::string(nebula::common::to_string(LogLevel::Debug)), std::string("DEBUG"), "debug text");
    expect_equal(std::string(nebula::common::to_string(LogLevel::Info)), std::string("INFO"), "info text");
    expect_equal(std::string(nebula::common::to_string(LogLevel::Warning)), std::string("WARN"), "warn text");
    expect_equal(std::string(nebula::common::to_string(LogLevel::Error)), std::string("ERROR"), "error text");
    expect_equal(std::string(nebula::common::to_string(LogLevel::Fatal)), std::string("FATAL"), "fatal text");
}

void test_to_string_covers_domains() {
    expect_equal(std::string(nebula::common::to_string(LogDomain::App)), std::string("app"), "app text");
    expect_equal(std::string(nebula::common::to_string(LogDomain::Auth)), std::string("auth"), "auth text");
    expect_equal(std::string(nebula::common::to_string(LogDomain::Common)), std::string("common"), "common text");
    expect_equal(std::string(nebula::common::to_string(LogDomain::Http)), std::string("http"), "http text");
    expect_equal(std::string(nebula::common::to_string(LogDomain::Server)), std::string("server"), "server text");
    expect_equal(std::string(nebula::common::to_string(LogDomain::Storage)), std::string("storage"), "storage text");
    expect_equal(std::string(nebula::common::to_string(LogDomain::Test)), std::string("test"), "test text");
}

void test_log_filename_uses_daily_pattern() {
    const TempDir dir("nebula-logger-name");

    Logger::instance().initialize(LogLevel::Info, dir.path(), false);
    Logger::instance().info(nebula::common::LogDomain::Test, "server bootstrap");

    const std::filesystem::path log_file = find_single_regular_file(dir.path());
    const std::string filename = log_file.filename().string();
    const std::regex pattern(R"(^nebula-\d{4}-\d{2}-\d{2}\.log$)");
    expect_true(std::regex_match(filename, pattern), "log filename should match nebula-YYYY-MM-DD.log");
}

void test_min_level_filtering() {
    const TempDir dir("nebula-logger-level");

    Logger::instance().initialize(LogLevel::Info, dir.path(), false);
    Logger::instance().debug(nebula::common::LogDomain::Test, "debug should be filtered");
    Logger::instance().info(nebula::common::LogDomain::Test, "info should be written");
    Logger::instance().error(nebula::common::LogDomain::Test, "error should be written");

    const std::string content = read_all(find_single_regular_file(dir.path()));
    expect_contains(content, "] [INFO] info should be written", "info log should exist");
    expect_contains(content, "] [ERROR] error should be written", "error log should exist");
    expect_not_contains(content, "debug should be filtered", "debug log should be filtered");
}

void test_iso8601_prefix_and_level_spacing() {
    const TempDir dir("nebula-logger-iso");

    Logger::instance().initialize(LogLevel::Info, dir.path(), false);
    Logger::instance().info(nebula::common::LogDomain::Test, "server started").field("port", 8080);

    const std::string content = read_all(find_single_regular_file(dir.path()));
    const std::string line = first_line(content);
    const std::regex pattern(
        R"(^\[\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}(Z|[+-]\d{2}:\d{2}(:\d{2})?)\] \[INFO\] server started: domain=test, port=8080$)");
    expect_true(std::regex_match(line, pattern), "prefix should be ISO 8601 and contain '] [INFO]'");
}

void test_structured_fields_format() {
    const TempDir dir("nebula-logger-structured");

    Logger::instance().initialize(LogLevel::Trace, dir.path(), false);
    Logger::instance()
        .warn(nebula::common::LogDomain::Test, "accept failed")
        .field("fd", 12)
        .field("peer", "127.0.0.1:54321")
        .field("decision", "keep_running");

    const std::string content = read_all(find_single_regular_file(dir.path()));
    expect_contains(content, "accept failed: domain=test, fd=12, peer=127.0.0.1:54321, decision=keep_running",
                    "structured format should match");
}

void test_domain_overload_adds_domain_field() {
    const TempDir dir("nebula-logger-domain-overload");

    Logger::instance().initialize(LogLevel::Trace, dir.path(), false);
    Logger::instance().warn(LogDomain::Auth, "login rejected").field("error", "invalid_request_body");

    const std::string content = read_all(find_single_regular_file(dir.path()));
    expect_contains(content, "login rejected: domain=auth, error=invalid_request_body",
                    "domain overload should append domain field");
}

void test_entry_logs_on_destruction() {
    const TempDir dir("nebula-logger-entry");

    Logger::instance().initialize(LogLevel::Trace, dir.path(), false);
    Logger::instance()
        .warn(nebula::common::LogDomain::Test, "accept failed")
        .field("fd", 12)
        .field("decision", "keep_running");

    const std::string content = read_all(find_single_regular_file(dir.path()));
    expect_contains(content, "accept failed: domain=test, fd=12, decision=keep_running",
                    "entry should emit log when scope exits");
}

void test_entry_emit_does_not_duplicate() {
    const TempDir dir("nebula-logger-entry-emit-once");

    Logger::instance().initialize(LogLevel::Trace, dir.path(), false);
    Logger::instance()
        .warn(nebula::common::LogDomain::Test, "accept failed")
        .field("fd", 12)
        .field("decision", "keep_running")
        .emit();

    const std::string content = read_all(find_single_regular_file(dir.path()));
    const std::string log_snippet = "accept failed: domain=test, fd=12, decision=keep_running";
    expect_equal(count_occurrences(content, log_snippet), static_cast<std::size_t>(1),
                 ".emit should not duplicate log in destructor");
}

void test_field_message_format() {
    const TempDir dir("nebula-logger-field-message");

    Logger::instance().initialize(LogLevel::Trace, dir.path(), false);
    Logger::instance()
        .info(nebula::common::LogDomain::Test, "request completed")
        .field("status", 405, "Method Not Allowed")
        .field("fd", 6);

    const std::string content = read_all(find_single_regular_file(dir.path()));
    expect_contains(content, "request completed: domain=test, status=405 (Method Not Allowed), fd=6",
                    "field message should render as key=value (message)");
}

void test_bool_field_formats_true_false() {
    const TempDir dir("nebula-logger-bool-field");

    Logger::instance().initialize(LogLevel::Trace, dir.path(), false);
    Logger::instance()
        .info(nebula::common::LogDomain::Test, "runtime state")
        .field("stop_requested", true)
        .field("running", false);

    const std::string content = read_all(find_single_regular_file(dir.path()));
    expect_contains(content, "runtime state: domain=test, stop_requested=true, running=false",
                    "bool field should render as true/false text");
}

void test_errno_with_message_formats_message() {
    const TempDir dir("nebula-logger-errno-message");

    Logger::instance().initialize(LogLevel::Trace, dir.path(), false);
    Logger::instance()
        .error(nebula::common::LogDomain::Test, "accept failed")
        .field("errno", 9, "Bad file descriptor")
        .field("decision", "keep_running");

    const std::string content = read_all(find_single_regular_file(dir.path()));
    expect_contains(content, "accept failed: domain=test, errno=9 (Bad file descriptor), decision=keep_running",
                    "errno field should render caller-provided message and structured decision");
    expect_not_contains(content, "accept failed: domain=test, errno=9, decision=keep_running",
                        "errno should include caller-provided error text");
}

void test_reinit_switches_output_directory() {
    const TempDir first_dir("nebula-logger-reinit-a");
    const TempDir second_dir("nebula-logger-reinit-b");

    Logger::instance().initialize(LogLevel::Info, first_dir.path(), false);
    Logger::instance().info(nebula::common::LogDomain::Test, "first directory message");

    Logger::instance().initialize(LogLevel::Info, second_dir.path(), false);
    Logger::instance().info(nebula::common::LogDomain::Test, "second directory message");

    const std::string first_content = read_all(find_single_regular_file(first_dir.path()));
    const std::string second_content = read_all(find_single_regular_file(second_dir.path()));
    expect_contains(first_content, "first directory message", "first message should remain in first directory");
    expect_not_contains(first_content, "second directory message",
                        "second message should not leak into first directory");
    expect_contains(second_content, "second directory message", "second message should exist in second directory");
}

void test_init_reports_create_directory_failure() {
    const TempDir dir("nebula-logger-create-dir-fail");
    const std::filesystem::path blocked_path = dir.path() / "blocked-dir";
    {
        std::ofstream blocked_file(blocked_path);
        expect_true(blocked_file.is_open(), "blocked file should be created");
    }

    const std::string stderr_text = capture_stderr([&]() {
        Logger::instance().initialize(LogLevel::Info, blocked_path.string(), false);
        Logger::instance().info(nebula::common::LogDomain::Test, "directory failure should still emit business log");
        Logger::instance().info(nebula::common::LogDomain::Test, "directory failure should keep emitting business log");
    });
    expect_contains(stderr_text,
                    "[ERROR] create log directory failed: path=", "create directory failure should be logged");
    expect_equal(count_occurrences(stderr_text, "[ERROR] create log directory failed: path="),
                 static_cast<std::size_t>(1), "create directory failure should only be logged once per day");
    expect_contains(stderr_text, "errno=", "create directory failure should include errno");
    expect_contains(stderr_text, ", fallback=stderr_only",
                    "create directory failure should include structured fallback");
    expect_contains(stderr_text, "] [INFO] directory failure should still emit business log",
                    "create directory failure should fallback to stderr business log");
    expect_contains(stderr_text, "] [INFO] directory failure should keep emitting business log",
                    "fallback mode should keep printing later business logs");
}

void test_init_reports_open_file_failure() {
    const TempDir dir("nebula-logger-open-fail");
    const std::filesystem::path blocked_file_path =
        dir.path() / std::format("nebula-{}.log", logger_current_date_text());
    std::error_code ec;
    std::filesystem::create_directories(blocked_file_path, ec);
    expect_true(!ec, "blocked path directory should be created");

    const std::string stderr_text = capture_stderr([&]() {
        Logger::instance().initialize(LogLevel::Info, dir.path(), false);
        Logger::instance().info(nebula::common::LogDomain::Test, "should fallback to stderr only");
        Logger::instance().info(nebula::common::LogDomain::Test, "fallback should keep printing business logs");
    });
    expect_contains(stderr_text, "[ERROR] open log file failed: path=", "open file failure should be logged");
    expect_equal(count_occurrences(stderr_text, "[ERROR] open log file failed: path="), static_cast<std::size_t>(1),
                 "open file failure should only be logged once per day");
    expect_contains(stderr_text, "errno=", "open file failure should include errno");
    expect_contains(stderr_text, ", fallback=stderr_only", "open file failure should include structured fallback");
    expect_contains(stderr_text, "] [INFO] should fallback to stderr only",
                    "open failure should fallback to stderr business log");
    expect_contains(stderr_text, "] [INFO] fallback should keep printing business logs",
                    "fallback mode should keep printing later business logs");
    expect_equal(count_regular_files(dir.path()), static_cast<std::size_t>(0),
                 "open failure should not create regular log file");
}

int run_logger_tests() {
    const std::vector<nebula::testsupport::TestCase> tests = {
        {"to_string covers all levels", test_to_string_covers_levels},
        {"to_string covers all domains", test_to_string_covers_domains},
        {"log filename uses daily pattern", test_log_filename_uses_daily_pattern},
        {"min level filtering", test_min_level_filtering},
        {"iso8601 prefix and level spacing", test_iso8601_prefix_and_level_spacing},
        {"structured fields format", test_structured_fields_format},
        {"domain overload adds domain field", test_domain_overload_adds_domain_field},
        {"entry logs on destruction", test_entry_logs_on_destruction},
        {"entry emit does not duplicate", test_entry_emit_does_not_duplicate},
        {"field message format", test_field_message_format},
        {"bool field formats true false", test_bool_field_formats_true_false},
        {"errno with message formats message", test_errno_with_message_formats_message},
        {"reinit switches output directory", test_reinit_switches_output_directory},
        {"init reports create directory failure", test_init_reports_create_directory_failure},
        {"init reports open file failure", test_init_reports_open_file_failure},
    };

    return nebula::testsupport::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::testsupport::run_main(run_logger_tests);
}
