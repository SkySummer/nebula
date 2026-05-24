#include <string>
#include <vector>

#include "nebula/common/log/types.hpp"
#include "nebula_tests/common.hpp"

namespace {

using namespace nebula;

void test_to_string_covers_levels() {
    test::expect_equal(nebula::common::to_string(common::LogLevel::Trace), std::string("TRACE"), "trace text");
    test::expect_equal(nebula::common::to_string(common::LogLevel::Debug), std::string("DEBUG"), "debug text");
    test::expect_equal(nebula::common::to_string(common::LogLevel::Info), std::string("INFO"), "info text");
    test::expect_equal(nebula::common::to_string(common::LogLevel::Warning), std::string("WARNING"), "warning text");
    test::expect_equal(nebula::common::to_string(common::LogLevel::Error), std::string("ERROR"), "error text");
    test::expect_equal(nebula::common::to_string(common::LogLevel::Fatal), std::string("FATAL"), "fatal text");
}

void test_parse_log_level_covers_supported_values() {
    const std::optional<common::LogLevel> trace_level = nebula::common::parse_log_level("TRACE");
    test::expect_equal(trace_level, std::optional<common::LogLevel>{common::LogLevel::Trace},
                       "trace should parse case-insensitively");

    const std::optional<common::LogLevel> debug_level = nebula::common::parse_log_level("debug");
    test::expect_equal(debug_level, std::optional<common::LogLevel>{common::LogLevel::Debug}, "debug level should map");

    const std::optional<common::LogLevel> warning_level = nebula::common::parse_log_level("Warning");
    test::expect_equal(warning_level, std::optional<common::LogLevel>{common::LogLevel::Warning},
                       "warning level should map");

    const std::optional<common::LogLevel> warn_level = nebula::common::parse_log_level("warn");
    test::expect_equal(warn_level, std::optional<common::LogLevel>{common::LogLevel::Warning},
                       "warn alias should map to warning");

    const std::optional<common::LogLevel> error_level = nebula::common::parse_log_level("error");
    test::expect_equal(error_level, std::optional<common::LogLevel>{common::LogLevel::Error}, "error level should map");

    const std::optional<common::LogLevel> fatal_level = nebula::common::parse_log_level("fatal");
    test::expect_equal(fatal_level, std::optional<common::LogLevel>{common::LogLevel::Fatal}, "fatal level should map");

    const std::optional<common::LogLevel> verbose_level = nebula::common::parse_log_level("verbose");
    test::expect_true(!verbose_level.has_value(), "unknown level should be rejected");
}

int run_logger_types_tests() {
    const std::vector<nebula::test::TestCase> tests = {
        {"to_string covers all levels", test_to_string_covers_levels},
        {"parse log level covers supported values", test_parse_log_level_covers_supported_values},
    };

    return nebula::test::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::test::run_main(run_logger_types_tests);
}
