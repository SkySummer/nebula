#include <cstdint>
#include <string>
#include <vector>

#include "nebula/common/codec/json.hpp"
#include "nebula/http/codec/response_writer.hpp"
#include "nebula/http/protocol/error.hpp"
#include "nebula_tests/common.hpp"

namespace {

using namespace nebula;

void test_api_success_response_shape() {
    nebula::common::JsonObject data;
    data.emplace("value", std::int64_t{1});

    const http::HttpResponse response = nebula::http::make_api_success_response(std::move(data));
    test::expect_true(response.status == http::HttpStatus::OK, "api success status should be 200");
    test::expect_contains(response.body, R"({"code":"ok","data":{"value":1},"message":"success"})",
                          "api success body should follow code-message-data schema");
    test::expect_true(response.headers.contains("Content-Type"), "api success should include content-type");
    test::expect_true(response.headers.at("Content-Type") == "application/json; charset=utf-8",
                      "api success content-type should be json utf-8");
}

void test_api_error_response_shape() {
    const http::HttpResponse response =
        nebula::http::make_api_error_response(http::HttpStatus::BadRequest, "invalid_request", "invalid request body");
    test::expect_true(response.status == http::HttpStatus::BadRequest, "api error status should match");
    test::expect_contains(response.body, R"({"code":"invalid_request","data":null,"message":"invalid request body"})",
                          "api error body should follow code-message-data schema");
    test::expect_true(response.headers.contains("Content-Type"), "api error should include content-type");
    test::expect_true(response.headers.at("Content-Type") == "application/json; charset=utf-8",
                      "api error content-type should be json utf-8");
}

void test_api_status_error_response_shape() {
    const http::HttpResponse response = nebula::http::make_api_error_response(http::HttpStatus::MethodNotAllowed);
    test::expect_true(response.status == http::HttpStatus::MethodNotAllowed, "api status error status should match");
    test::expect_contains(response.body, R"({"code":"method_not_allowed","data":null,"message":"method not allowed"})",
                          "api status error body should use fixed status mapping");
    test::expect_true(response.headers.contains("Content-Type"), "api status error should include content-type");
    test::expect_true(response.headers.at("Content-Type") == "application/json; charset=utf-8",
                      "api status error content-type should be json utf-8");
}

void test_http_status_api_error_mapping() {
    const nebula::http::HttpErrorInfo bad_request = nebula::http::to_error_info(http::HttpStatus::BadRequest);
    test::expect_true(bad_request.status == http::HttpStatus::BadRequest, "bad request should keep mapped status");
    test::expect_equal(bad_request.code, std::string("bad_request"), "bad request should map to bad_request code");
    test::expect_equal(bad_request.message, std::string("bad request"), "bad request should map to api message");

    const nebula::http::HttpErrorInfo method_not_allowed =
        nebula::http::to_error_info(http::HttpStatus::MethodNotAllowed);
    test::expect_true(method_not_allowed.status == http::HttpStatus::MethodNotAllowed,
                      "method not allowed should keep mapped status");
    test::expect_equal(method_not_allowed.code, std::string("method_not_allowed"),
                       "method not allowed should map to method_not_allowed code");
    test::expect_equal(method_not_allowed.message, std::string("method not allowed"),
                       "method not allowed should map to api message");

    const nebula::http::HttpErrorInfo internal_error =
        nebula::http::to_error_info(http::HttpStatus::InternalServerError);
    test::expect_true(internal_error.status == http::HttpStatus::InternalServerError,
                      "internal server error should keep mapped status");
    test::expect_equal(internal_error.code, std::string("internal_server_error"),
                       "internal server error should map to internal_server_error code");
    test::expect_equal(internal_error.message, std::string("internal server error"),
                       "internal server error should map to api message");
}

void test_http_status_api_error_mapping_rejects_non_error_status() {
    const nebula::http::HttpErrorInfo ok = nebula::http::to_error_info(http::HttpStatus::OK);
    test::expect_true(ok.status == http::HttpStatus::InternalServerError, "non-error status should normalize to 500");
    test::expect_equal(ok.code, std::string("internal_server_error"),
                       "non-error status should map to internal_server_error code");
    test::expect_equal(ok.message, std::string("internal server error"),
                       "non-error status should map to internal error message");

    const http::HttpResponse response = nebula::http::make_api_error_response(http::HttpStatus::OK);
    test::expect_true(response.status == http::HttpStatus::InternalServerError,
                      "api error response should not emit 2xx status");
    test::expect_contains(response.body,
                          R"({"code":"internal_server_error","data":null,"message":"internal server error"})",
                          "api error response should use normalized error info");
}

void test_api_status_error_response_custom_message() {
    const http::HttpResponse response =
        nebula::http::make_api_error_response(http::HttpStatus::BadRequest, "Missing Host Header");
    test::expect_true(response.status == http::HttpStatus::BadRequest,
                      "api status error with message status should match");
    test::expect_contains(response.body, R"({"code":"bad_request","data":null,"message":"Missing Host Header"})",
                          "api status error should keep status code mapping with custom message");
}

void test_api_internal_error_response_shape() {
    const http::HttpResponse response = nebula::http::make_api_error_response(http::HttpStatus::InternalServerError);
    test::expect_true(response.status == http::HttpStatus::InternalServerError,
                      "api internal error status should be 500");
    test::expect_contains(response.body,
                          R"({"code":"internal_server_error","data":null,"message":"internal server error"})",
                          "api internal error body should follow fixed internal_server_error contract");
    test::expect_true(response.headers.contains("Content-Type"), "api internal error should include content-type");
    test::expect_true(response.headers.at("Content-Type") == "application/json; charset=utf-8",
                      "api internal error content-type should be json utf-8");
}

int run_api_response_contract_tests() {
    const std::vector<nebula::test::TestCase> tests = {
        {"api success response shape", test_api_success_response_shape},
        {"api error response shape", test_api_error_response_shape},
        {"api status error response shape", test_api_status_error_response_shape},
        {"http status api error mapping", test_http_status_api_error_mapping},
        {"http status api error mapping rejects non-error status",
         test_http_status_api_error_mapping_rejects_non_error_status},
        {"api status error response custom message", test_api_status_error_response_custom_message},
        {"api internal error response shape", test_api_internal_error_response_shape},
    };
    return nebula::test::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::test::run_main(run_api_response_contract_tests);
}
