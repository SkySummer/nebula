#include <cstdint>
#include <vector>

#include "nebula/common/json.hpp"
#include "nebula/http/http_response_writer.hpp"
#include "nebula_tests/test_support.hpp"

namespace {

using nebula::http::HttpResponse;
using nebula::http::HttpStatus;
using nebula::testsupport::expect_contains;
using nebula::testsupport::expect_true;

void test_api_success_response_shape() {
    nebula::common::JsonObject data;
    data.emplace("value", nebula::common::JsonValue(static_cast<std::int64_t>(1)));

    const HttpResponse response = nebula::http::make_api_success_response(nebula::common::JsonValue(std::move(data)));
    expect_true(response.status == HttpStatus::OK, "api success status should be 200");
    expect_contains(response.body, R"({"code":"ok","data":{"value":1},"message":"success"})",
                    "api success body should follow code-message-data schema");
    expect_true(response.headers.contains("Content-Type"), "api success should include content-type");
    expect_true(response.headers.at("Content-Type") == "application/json; charset=utf-8",
                "api success content-type should be json utf-8");
}

void test_api_error_response_shape() {
    const HttpResponse response =
        nebula::http::make_api_error_response(HttpStatus::BadRequest, "invalid_request", "invalid request body");
    expect_true(response.status == HttpStatus::BadRequest, "api error status should match");
    expect_contains(response.body, R"({"code":"invalid_request","data":null,"message":"invalid request body"})",
                    "api error body should follow code-message-data schema");
    expect_true(response.headers.contains("Content-Type"), "api error should include content-type");
    expect_true(response.headers.at("Content-Type") == "application/json; charset=utf-8",
                "api error content-type should be json utf-8");
}

int run_api_response_contract_tests() {
    const std::vector<nebula::testsupport::TestCase> tests = {
        {"api success response shape", test_api_success_response_shape},
        {"api error response shape", test_api_error_response_shape},
    };
    return nebula::testsupport::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::testsupport::run_main(run_api_response_contract_tests);
}
