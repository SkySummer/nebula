#include "nebula/server/server_app.hpp"

#include <string>
#include <vector>

#include "nebula_tests/test_support.hpp"

namespace {

using nebula::testsupport::ArgvBuilder;
using nebula::testsupport::expect_contains;
using nebula::testsupport::expect_equal;
using nebula::testsupport::expect_true;
using nebula::testsupport::TempDir;
using nebula::testsupport::write_file;

int run_server_app_with_stderr(const std::vector<std::string>& args, std::string& stderr_output) {
    ArgvBuilder argv(args);

    int exit_code = -1;
    stderr_output = nebula::testsupport::capture_stderr(
        [&]() {
            nebula::server::ServerApp app(argv.span());
            exit_code = app.run();
        },
        "nebula-server-app-test");

    return exit_code;
}

void test_run_rejected_when_main_option_invalid() {
    std::string stderr_output;
    const int exit_code = run_server_app_with_stderr({"nebula", "--config"}, stderr_output);

    expect_equal(exit_code, 1, "invalid options should return non-zero");
    expect_contains(stderr_output, "parse main options failed", "should print startup parse failure");
    expect_contains(stderr_output, "decision=exit_process", "startup parse failure should include decision");
}

void test_run_rejected_when_required_config_missing() {
    const TempDir dir("nebula-server-app-missing-config");
    const auto missing_config = dir.path() / "missing.toml";

    std::string stderr_output;
    const int exit_code = run_server_app_with_stderr({"nebula", "--config", missing_config.string()}, stderr_output);

    expect_equal(exit_code, 1, "missing required config should return non-zero");
    expect_contains(stderr_output, "load server config failed", "should print config load failure");
    expect_contains(stderr_output, "decision=exit_process", "config load failure should include decision");
}

void test_run_rejected_when_root_default_source_missing_reports_error() {
    const TempDir dir("nebula-server-app-root-default-missing-source");
    const auto config_file = dir.path() / "config.toml";
    write_file(config_file,
               "[routes]\n"
               "enable_healthz = false\n"
               "enable_root_default = true\n"
               "root_default_path = \"/healthz\"\n");

    std::string stderr_output;
    const int exit_code = run_server_app_with_stderr({"nebula", "--config", config_file.string()}, stderr_output);

    expect_equal(exit_code, 1, "missing root default source route should return non-zero");
    expect_contains(stderr_output, "register default route failed",
                    "root default registration failure should be logged");
    expect_contains(stderr_output, "error=source_get_route_not_found",
                    "root default registration failure should include error");
    expect_contains(stderr_output, "decision=exit_process",
                    "root default registration failure should include decision");
}

void test_route_management_apis() {
    ArgvBuilder argv(std::vector<std::string>{"nebula"});
    nebula::server::ServerApp app(argv.span());

    expect_true(app.add_route(nebula::http::HttpMethod::Get, "/custom",
                              [](const nebula::http::RouteContext&) {
                                  nebula::http::HttpResponse response;
                                  response.status = nebula::http::HttpStatus::OK;
                                  response.body = "v1";
                                  return response;
                              }),
                "add custom route should succeed");
    expect_true(app.has_route_exact(nebula::http::HttpMethod::Get, "/custom"),
                "has route exact should find added route");

    expect_true(app.mod_route(nebula::http::HttpMethod::Get, "/custom",
                              [](const nebula::http::RouteContext&) {
                                  nebula::http::HttpResponse response;
                                  response.status = nebula::http::HttpStatus::OK;
                                  response.body = "v2";
                                  return response;
                              }),
                "mod custom route should succeed");

    expect_true(app.del_route(nebula::http::HttpMethod::Get, "/custom"), "del custom route should succeed");
    expect_true(!app.has_route_exact(nebula::http::HttpMethod::Get, "/custom"),
                "has route exact should miss deleted route");
}

void test_route_management_source_path_apis() {
    ArgvBuilder argv(std::vector<std::string>{"nebula"});
    nebula::server::ServerApp app(argv.span());

    expect_true(app.add_route(nebula::http::HttpMethod::Get, "/source",
                              [](const nebula::http::RouteContext&) {
                                  nebula::http::HttpResponse response;
                                  response.status = nebula::http::HttpStatus::OK;
                                  response.body = "source";
                                  return response;
                              }),
                "add source route should succeed");
    expect_true(app.add_route(nebula::http::HttpMethod::Get, "/alias", "/source"),
                "add alias route from source path should succeed");
    expect_true(app.has_route_exact(nebula::http::HttpMethod::Get, "/alias"),
                "has route exact should find alias route");
    expect_true(app.mod_route(nebula::http::HttpMethod::Get, "/alias", "/source"),
                "mod alias route from source path should succeed");
}

void test_has_route_match_and_exact() {
    ArgvBuilder argv(std::vector<std::string>{"nebula"});
    nebula::server::ServerApp app(argv.span());

    expect_true(app.add_route(nebula::http::HttpMethod::Get, "/users/{id}",
                              [](const nebula::http::RouteContext&) {
                                  nebula::http::HttpResponse response;
                                  response.status = nebula::http::HttpStatus::OK;
                                  response.body = "ok";
                                  return response;
                              }),
                "add dynamic route should succeed");

    expect_true(app.has_route_exact(nebula::http::HttpMethod::Get, "/users/{id}"),
                "has route exact should match template path");
    expect_true(!app.has_route_exact(nebula::http::HttpMethod::Get, "/users/42"),
                "has route exact should not match concrete path");
    expect_true(app.has_route_match(nebula::http::HttpMethod::Get, "/users/42"),
                "has route match should match concrete dynamic path");
    expect_true(!app.has_route_match(nebula::http::HttpMethod::Post, "/users/42"),
                "has route match should reject wrong method");
}

void test_root_default_registered_in_run_phase() {
    ArgvBuilder argv(std::vector<std::string>{"nebula"});
    nebula::server::ServerApp app(argv.span());

    expect_true(!app.has_route_exact(nebula::http::HttpMethod::Get, "/"),
                "root default route should not be pre-registered before run");
    expect_true(app.has_route_exact(nebula::http::HttpMethod::Get, "/healthz"),
                "healthz route should still be pre-registered");
}

void test_route_management_apis_rejected_when_startup_invalid() {
    ArgvBuilder argv(std::vector<std::string>{"nebula", "--config"});
    nebula::server::ServerApp app(argv.span());

    expect_true(!app.add_route(nebula::http::HttpMethod::Get, "/x",
                               [](const nebula::http::RouteContext&) {
                                   nebula::http::HttpResponse response;
                                   response.status = nebula::http::HttpStatus::OK;
                                   return response;
                               }),
                "add route should fail when startup invalid");
    expect_true(!app.mod_route(nebula::http::HttpMethod::Get, "/x",
                               [](const nebula::http::RouteContext&) {
                                   nebula::http::HttpResponse response;
                                   response.status = nebula::http::HttpStatus::OK;
                                   return response;
                               }),
                "mod route should fail when startup invalid");
    expect_true(!app.del_route(nebula::http::HttpMethod::Get, "/x"), "del route should fail when startup invalid");
    expect_true(!app.has_route_exact(nebula::http::HttpMethod::Get, "/x"),
                "has route exact should fail when startup invalid");
    expect_true(!app.has_route_match(nebula::http::HttpMethod::Get, "/x"),
                "has route match should fail when startup invalid");
}

int run_server_app_tests() {
    const std::vector<nebula::testsupport::TestCase> tests = {
        {"run rejected when main option invalid", test_run_rejected_when_main_option_invalid},
        {"run rejected when required config missing", test_run_rejected_when_required_config_missing},
        {"run rejected when root default source missing reports error",
         test_run_rejected_when_root_default_source_missing_reports_error},
        {"route management apis", test_route_management_apis},
        {"route management source path apis", test_route_management_source_path_apis},
        {"has route match and exact", test_has_route_match_and_exact},
        {"root default registered in run phase", test_root_default_registered_in_run_phase},
        {"route management apis rejected when startup invalid",
         test_route_management_apis_rejected_when_startup_invalid},
    };

    return nebula::testsupport::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::testsupport::run_main(run_server_app_tests);
}
