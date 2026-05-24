#include "nebula/common/log/logger.hpp"

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "nebula/common/codec/json.hpp"
#include "nebula_tests/common.hpp"

namespace {

using namespace nebula;

struct ParsedLogLine {
    std::string ts;
    std::string level;
    std::string event;
    std::unordered_map<std::string, std::string> fields;
};

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

ParsedLogLine parse_log_line(std::string_view line) {
    if (line.empty() || line.front() != '[') {
        test::fail(std::format("log line missing timestamp bracket: {}", std::string(line)));
    }

    const std::size_t timestamp_separator = line.find("] [");
    if (timestamp_separator == std::string_view::npos) {
        test::fail(std::format("log line missing timestamp separator: {}", std::string(line)));
    }

    const std::size_t level_open = timestamp_separator + 2U;
    const std::size_t level_close = line.find("] ", level_open);
    if (level_close == std::string_view::npos) {
        test::fail(std::format("log line missing level separator: {}", std::string(line)));
    }

    ParsedLogLine parsed = {
        .ts = std::string(line.substr(1U, timestamp_separator - 1U)),
        .level = std::string(line.substr(level_open + 1U, level_close - level_open - 1U)),
        .event = {},
        .fields = {},
    };

    const std::string_view remainder = line.substr(level_close + 2U);
    const std::size_t field_separator = remainder.find(": ");
    if (field_separator == std::string_view::npos) {
        parsed.event = std::string(remainder);
        return parsed;
    }

    parsed.event = std::string(remainder.substr(0, field_separator));
    std::size_t field_start = field_separator + 2U;
    while (field_start < remainder.size()) {
        const std::size_t field_end = remainder.find(", ", field_start);
        const std::string_view field_text = field_end == std::string_view::npos
                                                ? remainder.substr(field_start)
                                                : remainder.substr(field_start, field_end - field_start);
        const std::size_t equals = field_text.find('=');
        if (equals == std::string_view::npos) {
            test::fail(std::format("log field missing '=': {}", std::string(field_text)));
        }

        std::string value(field_text.substr(equals + 1U));
        if (!value.empty() && value.front() == '"') {
            const common::JsonParseResult parsed_value = common::parse_json(value);
            test::expect_true(parsed_value.ok, std::format("quoted field should parse as json string: {}", value));
            const std::string* string_value = parsed_value.value.get_if_string();
            test::expect_true(string_value != nullptr, std::format("quoted field should decode to string: {}", value));
            value = *string_value;
        }

        parsed.fields.emplace(std::string(field_text.substr(0, equals)), std::move(value));
        if (field_end == std::string_view::npos) {
            break;
        }
        field_start = field_end + 2U;
    }

    return parsed;
}

std::vector<ParsedLogLine> parse_log_lines(std::string_view text) {
    std::vector<ParsedLogLine> lines;

    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find('\n', start);
        const std::string_view line =
            end == std::string_view::npos ? text.substr(start) : text.substr(start, end - start);
        start = end == std::string_view::npos ? text.size() : end + 1U;

        if (line.empty()) {
            continue;
        }

        lines.push_back(parse_log_line(line));
    }

    return lines;
}

const std::string& require_field(const ParsedLogLine& line, std::string_view key) {
    if (key == "ts") {
        return line.ts;
    }
    if (key == "level") {
        return line.level;
    }
    if (key == "event") {
        return line.event;
    }

    const auto it = line.fields.find(std::string(key));
    if (it == line.fields.end()) {
        test::fail(std::format("missing field '{}'", key));
    }
    return it->second;
}

void expect_string_field(const ParsedLogLine& line, std::string_view key, std::string_view expected,
                         std::string_view message) {
    test::expect_equal(require_field(line, key), std::string(expected), message);
}

void expect_int_field(const ParsedLogLine& line, std::string_view key, std::int64_t expected,
                      std::string_view message) {
    const std::string& value = require_field(line, key);
    test::expect_equal(std::stoll(value), expected, message);
}

void expect_bool_field(const ParsedLogLine& line, std::string_view key, bool expected, std::string_view message) {
    test::expect_equal(require_field(line, key), std::string(expected ? "true" : "false"), message);
}

void expect_ts_field(const ParsedLogLine& line, std::string_view message) {
    const std::regex pattern(R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}(Z|[+-]\d{2}:\d{2}(:\d{2})?)$)");
    test::expect_true(std::regex_match(line.ts, pattern), message);
}

void test_uninitialized_logger_writes_all_levels_to_stderr_only() {
    const std::string stderr_text = test::capture_stderr([]() {
        common::Logger::instance().trace("trace before initialize").field("stage", "boot");
        common::Logger::instance().debug("debug before initialize").field("stage", "boot");
        common::Logger::instance().info("info before initialize").field("stage", "boot");
    });

    const std::vector<ParsedLogLine> lines = parse_log_lines(stderr_text);
    test::expect_equal(lines.size(), std::size_t{3}, "uninitialized logger should emit three lines");

    test::expect_equal(lines[0].level, std::string("TRACE"), "trace level should match");
    expect_string_field(lines[0], "event", "trace before initialize", "trace event should keep spaces");
    expect_string_field(lines[0], "stage", "boot", "trace stage should match");

    test::expect_equal(lines[1].level, std::string("DEBUG"), "debug level should match");
    expect_string_field(lines[1], "event", "debug before initialize", "debug event should keep spaces");

    test::expect_equal(lines[2].level, std::string("INFO"), "info level should match");
    expect_string_field(lines[2], "event", "info before initialize", "info event should keep spaces");
}

void test_log_filename_uses_daily_pattern() {
    const test::TempDir dir("nebula-logger-name");

    common::Logger::instance().initialize({.level = common::LogLevel::Info, .dir = dir.path(), .also_stderr = false});
    common::Logger::instance().info("server bootstrap");

    const std::filesystem::path log_file = test::find_single_regular_file(dir.path());
    const std::string filename = log_file.filename().generic_string();
    const std::regex pattern(R"(^nebula-\d{4}-\d{2}-\d{2}\.log$)");
    test::expect_true(std::regex_match(filename, pattern), "log filename should match nebula-YYYY-MM-DD.log");
}

void test_min_level_filtering() {
    const test::TempDir dir("nebula-logger-level");

    common::Logger::instance().initialize({.level = common::LogLevel::Info, .dir = dir.path(), .also_stderr = false});
    common::Logger::instance().debug("debug should be filtered");
    common::Logger::instance().info("info should be written");
    common::Logger::instance().error("error should be written");

    const std::vector<ParsedLogLine> lines =
        parse_log_lines(test::read_all(test::find_single_regular_file(dir.path())));
    test::expect_equal(lines.size(), std::size_t{2}, "filtered output should keep only info and error");
    expect_string_field(lines[0], "event", "info should be written", "info event should remain");
    test::expect_equal(lines[0].level, std::string("INFO"), "info level should remain");
    expect_string_field(lines[1], "event", "error should be written", "error event should remain");
    test::expect_equal(lines[1].level, std::string("ERROR"), "error level should remain");
}

void test_bracketed_iso8601_prefix_and_level_spacing() {
    const test::TempDir dir("nebula-logger-iso");

    common::Logger::instance().initialize({.level = common::LogLevel::Info, .dir = dir.path(), .also_stderr = false});
    common::Logger::instance().info("server started").field("port", 8080);

    const std::string raw_output = test::read_all(test::find_single_regular_file(dir.path()));
    const std::vector<ParsedLogLine> lines = parse_log_lines(raw_output);
    test::expect_equal(lines.size(), std::size_t{1}, "single log line should be written");
    expect_ts_field(lines.front(), "ts should use iso8601 with milliseconds");
    test::expect_equal(lines.front().level, std::string("INFO"), "info level should be uppercase");
    expect_string_field(lines.front(), "event", "server started", "event should keep spaces");
    expect_int_field(lines.front(), "port", 8080, "port should remain numeric");
    test::expect_contains(raw_output, "[", "raw output should include bracketed prefix");
    test::expect_contains(raw_output, "] [INFO] server started", "raw output should keep bracketed level prefix");
}

void test_structured_fields_format() {
    const test::TempDir dir("nebula-logger-structured");

    common::Logger::instance().initialize({.level = common::LogLevel::Trace, .dir = dir.path(), .also_stderr = false});
    common::Logger::instance()
        .warn("accept failed")
        .field("fd", 12)
        .field("peer", "127.0.0.1:54321")
        .field("decision", "keep_running");

    const ParsedLogLine line = parse_log_lines(test::read_all(test::find_single_regular_file(dir.path()))).front();
    test::expect_equal(line.level, std::string("WARNING"), "warn should serialize as WARNING");
    expect_string_field(line, "event", "accept failed", "event should keep spaces");
    expect_int_field(line, "fd", 12, "fd should remain numeric");
    expect_string_field(line, "peer", "127.0.0.1:54321", "peer should match");
    expect_string_field(line, "decision", "keep_running", "decision should match");
}

void test_entry_logs_on_destruction() {
    const test::TempDir dir("nebula-logger-entry");

    common::Logger::instance().initialize({.level = common::LogLevel::Trace, .dir = dir.path(), .also_stderr = false});
    common::Logger::instance().warn("accept failed").field("fd", 12).field("decision", "keep_running");

    const std::vector<ParsedLogLine> lines =
        parse_log_lines(test::read_all(test::find_single_regular_file(dir.path())));
    test::expect_equal(lines.size(), std::size_t{1}, "entry should emit once on destruction");
    expect_string_field(lines.front(), "event", "accept failed", "destruction emit event should match");
}

void test_entry_emit_does_not_duplicate() {
    const test::TempDir dir("nebula-logger-entry-emit-once");

    common::Logger::instance().initialize({.level = common::LogLevel::Trace, .dir = dir.path(), .also_stderr = false});
    common::Logger::instance().warn("accept failed").field("fd", 12).field("decision", "keep_running").emit();

    const std::vector<ParsedLogLine> lines =
        parse_log_lines(test::read_all(test::find_single_regular_file(dir.path())));
    test::expect_equal(lines.size(), std::size_t{1}, ".emit should not duplicate log in destructor");
}

void test_status_text_field_format() {
    const test::TempDir dir("nebula-logger-field-message");

    common::Logger::instance().initialize({.level = common::LogLevel::Trace, .dir = dir.path(), .also_stderr = false});
    common::Logger::instance()
        .info("request completed")
        .field("status", 405)
        .field("status_text", "Method Not Allowed")
        .field("fd", 6);

    const ParsedLogLine line = parse_log_lines(test::read_all(test::find_single_regular_file(dir.path()))).front();
    expect_string_field(line, "event", "request completed", "request event should keep spaces");
    expect_int_field(line, "status", 405, "status should remain numeric");
    expect_string_field(line, "status_text", "Method Not Allowed", "status text should be split into dedicated field");
    expect_int_field(line, "fd", 6, "fd should remain numeric");
}

void test_string_field_uses_json_escape_sequences() {
    const test::TempDir dir("nebula-logger-string-escape");

    common::Logger::instance().initialize({.level = common::LogLevel::Trace, .dir = dir.path(), .also_stderr = false});
    common::Logger::instance().info("escape text").field("message", "line1\nline2\t\"quoted\"\\tail");

    const std::string raw_output = test::read_all(test::find_single_regular_file(dir.path()));
    const ParsedLogLine line = parse_log_lines(raw_output).front();
    expect_string_field(line, "event", "escape text", "event should remain unescaped");
    expect_string_field(line, "message", "line1\nline2\t\"quoted\"\\tail",
                        "string field should round-trip through JSON escape sequences");
    test::expect_contains(raw_output, R"(message="line1\nline2\t\"quoted\"\\tail")",
                          "string field should be emitted as quoted JSON-escaped text");
}

void test_bool_field_formats_true_false() {
    const test::TempDir dir("nebula-logger-bool-field");

    common::Logger::instance().initialize({.level = common::LogLevel::Trace, .dir = dir.path(), .also_stderr = false});
    common::Logger::instance()
        .info("runtime state")
        .field("stop_requested", true)
        .field("running", false)
        .field("attempt", 3)
        .field("nothing", nullptr)
        .field("positive_limit", std::numeric_limits<double>::infinity())
        .field("negative_limit", -std::numeric_limits<double>::infinity())
        .field("not_a_number", std::numeric_limits<double>::quiet_NaN());

    const std::string raw_output = test::read_all(test::find_single_regular_file(dir.path()));
    const ParsedLogLine line = parse_log_lines(raw_output).front();
    expect_bool_field(line, "stop_requested", true, "stop_requested should be bool");
    expect_bool_field(line, "running", false, "running should be bool");
    expect_int_field(line, "attempt", 3, "attempt should remain numeric");
    expect_string_field(line, "nothing", "null", "nullptr should serialize as null literal");
    expect_string_field(line, "positive_limit", "inf", "positive infinity should use inf literal");
    expect_string_field(line, "negative_limit", "-inf", "negative infinity should use -inf literal");
    expect_string_field(line, "not_a_number", "nan", "nan should use nan literal");
    test::expect_contains(raw_output, "stop_requested=true", "bool true should stay unquoted");
    test::expect_contains(raw_output, "running=false", "bool false should stay unquoted");
    test::expect_contains(raw_output, "attempt=3", "integral field should stay unquoted");
    test::expect_contains(raw_output, "nothing=null", "nullptr field should use null literal");
    test::expect_contains(raw_output, "positive_limit=inf", "positive infinity should use inf literal");
    test::expect_contains(raw_output, "negative_limit=-inf", "negative infinity should use -inf literal");
    test::expect_contains(raw_output, "not_a_number=nan", "nan should use nan literal");
}

void test_errno_error_fields_format() {
    const test::TempDir dir("nebula-logger-errno-message");

    common::Logger::instance().initialize({.level = common::LogLevel::Trace, .dir = dir.path(), .also_stderr = false});
    common::Logger::instance()
        .error("accept failed")
        .field("errno", 9)
        .field("error", "Bad file descriptor")
        .field("decision", "keep_running");

    const ParsedLogLine line = parse_log_lines(test::read_all(test::find_single_regular_file(dir.path()))).front();
    expect_string_field(line, "event", "accept failed", "errno log event should match");
    expect_int_field(line, "errno", 9, "errno should remain numeric");
    expect_string_field(line, "error", "Bad file descriptor", "error field should keep errno text");
    expect_string_field(line, "decision", "keep_running", "decision should remain structured");
}

void test_reinit_switches_output_directory() {
    const test::TempDir first_dir("nebula-logger-reinit-a");
    const test::TempDir second_dir("nebula-logger-reinit-b");

    common::Logger::instance().initialize(
        {.level = common::LogLevel::Info, .dir = first_dir.path(), .also_stderr = false});
    common::Logger::instance().info("first directory message");

    common::Logger::instance().initialize(
        {.level = common::LogLevel::Info, .dir = second_dir.path(), .also_stderr = false});
    common::Logger::instance().info("second directory message");

    const ParsedLogLine first_line =
        parse_log_lines(test::read_all(test::find_single_regular_file(first_dir.path()))).front();
    const ParsedLogLine second_line =
        parse_log_lines(test::read_all(test::find_single_regular_file(second_dir.path()))).front();
    expect_string_field(first_line, "event", "first directory message", "first message should remain in first dir");
    expect_string_field(second_line, "event", "second directory message", "second message should remain in second dir");
}

void test_init_reports_create_directory_failure() {
    const test::TempDir dir("nebula-logger-create-dir-fail");
    const std::filesystem::path blocked_path = dir.path() / "blocked-dir";
    {
        std::ofstream blocked_file(blocked_path);
        test::expect_true(blocked_file.is_open(), "blocked file should be created");
    }

    const std::string stderr_text = test::capture_stderr([&]() {
        common::Logger::instance().initialize({
            .level = common::LogLevel::Info,
            .dir = blocked_path.generic_string(),
            .also_stderr = false,
        });
        common::Logger::instance().info("directory failure should still emit business log");
        common::Logger::instance().info("directory failure should keep emitting business log");
    });

    const std::vector<ParsedLogLine> lines = parse_log_lines(stderr_text);
    test::expect_equal(lines.size(), std::size_t{3}, "directory failure path should emit one error and two info lines");
    test::expect_equal(lines[0].level, std::string("ERROR"), "directory failure level should be error");
    expect_string_field(lines[0], "event", "create log directory failed", "directory failure event should match");
    expect_string_field(lines[0], "fallback", "stderr_only", "directory failure fallback should match");
    test::expect_true(!require_field(lines[0], "errno").empty(), "directory failure errno should be present");
    expect_string_field(lines[1], "event", "directory failure should still emit business log",
                        "business log should continue on stderr");
    expect_string_field(lines[2], "event", "directory failure should keep emitting business log",
                        "fallback mode should keep later business logs");
}

void test_init_reports_open_file_failure() {
    const test::TempDir dir("nebula-logger-open-fail");
    const std::filesystem::path blocked_file_path =
        dir.path() / std::format("nebula-{}.log", logger_current_date_text());
    std::error_code ec;
    std::filesystem::create_directories(blocked_file_path, ec);
    test::expect_true(!ec, "blocked path directory should be created");

    const std::string stderr_text = test::capture_stderr([&]() {
        common::Logger::instance().initialize(
            {.level = common::LogLevel::Info, .dir = dir.path(), .also_stderr = false});
        common::Logger::instance().info("should fallback to stderr only");
        common::Logger::instance().info("fallback should keep printing business logs");
    });

    const std::vector<ParsedLogLine> lines = parse_log_lines(stderr_text);
    test::expect_equal(lines.size(), std::size_t{3}, "open failure path should emit one error and two info lines");
    test::expect_equal(lines[0].level, std::string("ERROR"), "open failure level should be error");
    expect_string_field(lines[0], "event", "open log file failed", "open failure event should match");
    expect_string_field(lines[0], "fallback", "stderr_only", "open failure fallback should match");
    test::expect_true(!require_field(lines[0], "errno").empty(), "open failure errno should be present");
    expect_string_field(lines[1], "event", "should fallback to stderr only",
                        "open failure should fallback to stderr business log");
    expect_string_field(lines[2], "event", "fallback should keep printing business logs",
                        "fallback mode should keep printing later business logs");
    test::expect_equal(count_regular_files(dir.path()), std::size_t{0},
                       "open failure should not create regular log file");
}

int run_logger_tests() {
    const std::vector<nebula::test::TestCase> tests = {
        {"uninitialized logger writes all levels to stderr only",
         test_uninitialized_logger_writes_all_levels_to_stderr_only},
        {"log filename uses daily pattern", test_log_filename_uses_daily_pattern},
        {"min level filtering", test_min_level_filtering},
        {"bracketed iso8601 prefix and level spacing", test_bracketed_iso8601_prefix_and_level_spacing},
        {"structured fields format", test_structured_fields_format},
        {"entry logs on destruction", test_entry_logs_on_destruction},
        {"entry emit does not duplicate", test_entry_emit_does_not_duplicate},
        {"status text field format", test_status_text_field_format},
        {"string field uses json escape sequences", test_string_field_uses_json_escape_sequences},
        {"bool field formats true false", test_bool_field_formats_true_false},
        {"errno error fields format", test_errno_error_fields_format},
        {"reinit switches output directory", test_reinit_switches_output_directory},
        {"init reports create directory failure", test_init_reports_create_directory_failure},
        {"init reports open file failure", test_init_reports_open_file_failure},
    };

    return nebula::test::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::test::run_main(run_logger_tests);
}
