#include "nebula/database/connection_info.hpp"

#include <string>
#include <vector>

#include "nebula_tests/common.hpp"

namespace {

using namespace nebula;

void test_build_connection_info_formats_database_config_fields() {
    const database::DatabaseConfig config{
        .host = "127.0.0.1",
        .port = 15432,
        .name = "nebula_test",
        .user = "nebula_user",
        .password = "secret",
        .max_connections = 4,
        .connect_timeout_s = 5,
        .acquire_timeout_ms = 3000,
    };

    const std::string connection_info = database::build_connection_info(config);
    test::expect_equal(connection_info,
                       std::string("host='127.0.0.1' port=15432 dbname='nebula_test' user='nebula_user' "
                                   "password='secret' connect_timeout=5"),
                       "connection info should format expected postgres key value pairs");
}

void test_build_connection_info_escapes_quotes_and_backslashes() {
    const database::DatabaseConfig config{
        .host = "db\\host",
        .port = 5432,
        .name = "nebula's_db",
        .user = "user\\'name",
        .password = "pa'ss\\word",
        .max_connections = 4,
        .connect_timeout_s = 3,
        .acquire_timeout_ms = 3000,
    };

    const std::string connection_info = database::build_connection_info(config);
    test::expect_contains(connection_info, "host='db\\\\host'", "host should escape backslashes");
    test::expect_contains(connection_info, "dbname='nebula\\'s_db'", "dbname should escape single quotes");
    test::expect_contains(connection_info, R"(user='user\\\'name')", "user should escape quotes and backslashes");
    test::expect_contains(connection_info, R"(password='pa\'ss\\word')",
                          "password should escape quotes and backslashes");
}

int run_connection_info_tests() {
    const std::vector<nebula::test::TestCase> tests = {
        {"build connection info formats database config fields",
         test_build_connection_info_formats_database_config_fields},
        {"build connection info escapes quotes and backslashes",
         test_build_connection_info_escapes_quotes_and_backslashes},
    };

    return nebula::test::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::test::run_main(run_connection_info_tests);
}
