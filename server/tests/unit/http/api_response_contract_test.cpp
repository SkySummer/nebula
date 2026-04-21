#include <cstdint>
#include <string_view>
#include <vector>

#include "nebula/common/json.hpp"
#include "nebula/http/http_response_writer.hpp"
#include "nebula_tests/test_support.hpp"

namespace {

using nebula::http::HttpResponse;
using nebula::http::HttpStatus;
using nebula::testsupport::expect_contains;
using nebula::testsupport::expect_equal;
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

void test_api_status_error_response_shape() {
    const HttpResponse response = nebula::http::make_api_error_response(HttpStatus::MethodNotAllowed);
    expect_true(response.status == HttpStatus::MethodNotAllowed, "api status error status should match");
    expect_contains(response.body, R"({"code":"method_not_allowed","data":null,"message":"method not allowed"})",
                    "api status error body should use fixed status mapping");
    expect_true(response.headers.contains("Content-Type"), "api status error should include content-type");
    expect_true(response.headers.at("Content-Type") == "application/json; charset=utf-8",
                "api status error content-type should be json utf-8");
}

void test_http_status_api_error_mapping() {
    const nebula::http::HttpErrorInfo bad_request = nebula::http::to_error_info(HttpStatus::BadRequest);
    expect_true(bad_request.status == HttpStatus::BadRequest, "bad request should keep mapped status");
    expect_equal(bad_request.code, std::string_view("bad_request"), "bad request should map to bad_request code");
    expect_equal(bad_request.message, std::string_view("bad request"), "bad request should map to api message");

    const nebula::http::HttpErrorInfo method_not_allowed = nebula::http::to_error_info(HttpStatus::MethodNotAllowed);
    expect_true(method_not_allowed.status == HttpStatus::MethodNotAllowed,
                "method not allowed should keep mapped status");
    expect_equal(method_not_allowed.code, std::string_view("method_not_allowed"),
                 "method not allowed should map to method_not_allowed code");
    expect_equal(method_not_allowed.message, std::string_view("method not allowed"),
                 "method not allowed should map to api message");

    const nebula::http::HttpErrorInfo internal_error = nebula::http::to_error_info(HttpStatus::InternalServerError);
    expect_true(internal_error.status == HttpStatus::InternalServerError,
                "internal server error should keep mapped status");
    expect_equal(internal_error.code, std::string_view("internal_server_error"),
                 "internal server error should map to internal_server_error code");
    expect_equal(internal_error.message, std::string_view("internal server error"),
                 "internal server error should map to api message");
}

void test_http_status_api_error_mapping_rejects_non_error_status() {
    const nebula::http::HttpErrorInfo ok = nebula::http::to_error_info(HttpStatus::OK);
    expect_true(ok.status == HttpStatus::InternalServerError, "non-error status should normalize to 500");
    expect_equal(ok.code, std::string_view("internal_server_error"),
                 "non-error status should map to internal_server_error code");
    expect_equal(ok.message, std::string_view("internal server error"),
                 "non-error status should map to internal error message");

    const HttpResponse response = nebula::http::make_api_error_response(HttpStatus::OK);
    expect_true(response.status == HttpStatus::InternalServerError, "api error response should not emit 2xx status");
    expect_contains(response.body, R"({"code":"internal_server_error","data":null,"message":"internal server error"})",
                    "api error response should use normalized error info");
}

void test_api_status_error_response_custom_message() {
    const HttpResponse response = nebula::http::make_api_error_response(HttpStatus::BadRequest, "Missing Host Header");
    expect_true(response.status == HttpStatus::BadRequest, "api status error with message status should match");
    expect_contains(response.body, R"({"code":"bad_request","data":null,"message":"Missing Host Header"})",
                    "api status error should keep status code mapping with custom message");
}

void test_api_internal_error_response_shape() {
    const HttpResponse response = nebula::http::make_api_error_response(HttpStatus::InternalServerError);
    expect_true(response.status == HttpStatus::InternalServerError, "api internal error status should be 500");
    expect_contains(response.body, R"({"code":"internal_server_error","data":null,"message":"internal server error"})",
                    "api internal error body should follow fixed internal_server_error contract");
    expect_true(response.headers.contains("Content-Type"), "api internal error should include content-type");
    expect_true(response.headers.at("Content-Type") == "application/json; charset=utf-8",
                "api internal error content-type should be json utf-8");
}

int run_api_response_contract_tests() {
    const std::vector<nebula::testsupport::TestCase> tests = {
        {"api success response shape", test_api_success_response_shape},
        {"api error response shape", test_api_error_response_shape},
        {"api status error response shape", test_api_status_error_response_shape},
        {"http status api error mapping", test_http_status_api_error_mapping},
        {"http status api error mapping rejects non-error status",
         test_http_status_api_error_mapping_rejects_non_error_status},
        {"api status error response custom message", test_api_status_error_response_custom_message},
        {"api internal error response shape", test_api_internal_error_response_shape},
    };
    return nebula::testsupport::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::testsupport::run_main(run_api_response_contract_tests);
}
