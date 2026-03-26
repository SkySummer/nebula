#include "nebula/http/router.hpp"

#include <atomic>
#include <thread>
#include <vector>

#include "nebula/http/http_response_writer.hpp"
#include "nebula_tests/test_support.hpp"

namespace {

using nebula::http::HttpMethod;
using nebula::http::HttpRequest;
using nebula::http::HttpStatus;
using nebula::http::RouteStatus;
using nebula::testsupport::expect_equal;
using nebula::testsupport::expect_true;

void test_route_match() {
    nebula::http::Router router;
    expect_true(router.add_route(
                    HttpMethod::Get, "/healthz",
                    [](const HttpRequest&) { return nebula::http::make_plain_text_response(HttpStatus::OK, "ok"); }),
                "add route should succeed");

    HttpRequest request;
    request.method = HttpMethod::Get;
    request.path = "/healthz";

    const auto result = router.dispatch(request);
    expect_equal(result.status, RouteStatus::Matched, "route should match");
    expect_equal(result.response.status, HttpStatus::OK, "status should be 200");
}

void test_route_not_found() {
    nebula::http::Router router;
    expect_true(router.add_route(
                    HttpMethod::Get, "/healthz",
                    [](const HttpRequest&) { return nebula::http::make_plain_text_response(HttpStatus::OK, "ok"); }),
                "add route should succeed");

    HttpRequest request;
    request.method = HttpMethod::Get;
    request.path = "/unknown";

    const auto result = router.dispatch(request);
    expect_equal(result.status, RouteStatus::NotFound, "path should not match");
}

void test_method_not_allowed() {
    nebula::http::Router router;
    expect_true(router.add_route(
                    HttpMethod::Get, "/echo",
                    [](const HttpRequest&) { return nebula::http::make_plain_text_response(HttpStatus::OK, "ok"); }),
                "add route should succeed");

    HttpRequest request;
    request.method = HttpMethod::Post;
    request.path = "/echo";

    const auto result = router.dispatch(request);
    expect_equal(result.status, RouteStatus::MethodNotAllowed, "method should not match");
    expect_equal(result.allowed_methods, std::vector<HttpMethod>{HttpMethod::Get},
                 "method not allowed should return allow-list methods");
}

void test_add_route_no_duplicate() {
    nebula::http::Router router;
    expect_true(router.add_route(
                    HttpMethod::Get, "/echo",
                    [](const HttpRequest&) { return nebula::http::make_plain_text_response(HttpStatus::OK, "v1"); }),
                "first add should succeed");
    expect_true(!router.add_route(
                    HttpMethod::Get, "/echo",
                    [](const HttpRequest&) { return nebula::http::make_plain_text_response(HttpStatus::OK, "v2"); }),
                "duplicate add should fail");

    HttpRequest request;
    request.method = HttpMethod::Get;
    request.path = "/echo";
    const auto result = router.dispatch(request);
    expect_equal(result.status, RouteStatus::Matched, "duplicate add failure should keep original route");
    expect_equal(result.response.body, std::string("v1"), "duplicate add should not overwrite original handler");
}

void test_mod_route() {
    nebula::http::Router router;
    expect_true(router.add_route(HttpMethod::Get, "/echo",
                                 [](const HttpRequest&) {
                                     return nebula::http::make_plain_text_response(HttpStatus::OK, "before");
                                 }),
                "add route should succeed");
    expect_true(router.mod_route(
                    HttpMethod::Get, "/echo",
                    [](const HttpRequest&) { return nebula::http::make_plain_text_response(HttpStatus::OK, "after"); }),
                "mod existing route should succeed");
    expect_true(!router.mod_route(
                    HttpMethod::Post, "/echo",
                    [](const HttpRequest&) { return nebula::http::make_plain_text_response(HttpStatus::OK, "nope"); }),
                "mod missing method should fail");
    expect_true(!router.mod_route(
                    HttpMethod::Get, "/missing",
                    [](const HttpRequest&) { return nebula::http::make_plain_text_response(HttpStatus::OK, "nope"); }),
                "mod missing path should fail");

    HttpRequest request;
    request.method = HttpMethod::Get;
    request.path = "/echo";
    const auto result = router.dispatch(request);
    expect_equal(result.status, RouteStatus::Matched, "mod route should keep route matched");
    expect_equal(result.response.body, std::string("after"), "mod route should replace handler");
}

void test_add_route_reject_empty_handler() {
    nebula::http::Router router;

    expect_true(!router.add_route(HttpMethod::Get, "/empty", nebula::http::Router::Handler{}),
                "add route with empty handler should fail");

    HttpRequest request;
    request.method = HttpMethod::Get;
    request.path = "/empty";
    const auto result = router.dispatch(request);
    expect_equal(result.status, RouteStatus::NotFound, "failed add should not create route");
}

void test_mod_route_reject_empty_handler() {
    nebula::http::Router router;
    expect_true(router.add_route(
                    HttpMethod::Get, "/echo",
                    [](const HttpRequest&) { return nebula::http::make_plain_text_response(HttpStatus::OK, "keep"); }),
                "add route should succeed");

    expect_true(!router.mod_route(HttpMethod::Get, "/echo", nebula::http::Router::Handler{}),
                "mod route with empty handler should fail");

    HttpRequest request;
    request.method = HttpMethod::Get;
    request.path = "/echo";
    const auto result = router.dispatch(request);
    expect_equal(result.status, RouteStatus::Matched, "failed mod should keep existing route");
    expect_equal(result.response.body, std::string("keep"), "failed mod should keep original handler");
}

void test_reject_empty_path() {
    nebula::http::Router router;

    expect_true(!router.add_route(
                    HttpMethod::Get, "",
                    [](const HttpRequest&) { return nebula::http::make_plain_text_response(HttpStatus::OK, "ok"); }),
                "add route with empty path should fail");

    expect_true(!router.mod_route(
                    HttpMethod::Get, "",
                    [](const HttpRequest&) { return nebula::http::make_plain_text_response(HttpStatus::OK, "ok"); }),
                "mod route with empty path should fail");

    expect_true(!router.del_route(HttpMethod::Get, ""), "del route with empty path should fail");

    HttpRequest request;
    request.method = HttpMethod::Get;
    request.path = "";
    const auto result = router.dispatch(request);
    expect_equal(result.status, RouteStatus::NotFound, "empty path should not be routable");
}

void test_del_route() {
    nebula::http::Router router;
    expect_true(router.add_route(
                    HttpMethod::Get, "/echo",
                    [](const HttpRequest&) { return nebula::http::make_plain_text_response(HttpStatus::OK, "get"); }),
                "add get route should succeed");
    expect_true(router.add_route(
                    HttpMethod::Post, "/echo",
                    [](const HttpRequest&) { return nebula::http::make_plain_text_response(HttpStatus::OK, "post"); }),
                "add post route should succeed");

    expect_true(router.del_route(HttpMethod::Get, "/echo"), "del existing method should succeed");

    HttpRequest get_request;
    get_request.method = HttpMethod::Get;
    get_request.path = "/echo";
    const auto get_result = router.dispatch(get_request);
    expect_equal(get_result.status, RouteStatus::MethodNotAllowed, "remaining methods should keep path alive");
    expect_equal(get_result.allowed_methods, std::vector<HttpMethod>{HttpMethod::Post},
                 "remaining methods should be present in allow-list");

    HttpRequest post_request;
    post_request.method = HttpMethod::Post;
    post_request.path = "/echo";
    const auto post_result = router.dispatch(post_request);
    expect_equal(post_result.status, RouteStatus::Matched, "other method should remain after partial remove");

    expect_true(router.del_route(HttpMethod::Post, "/echo"), "del last method should succeed");
    const auto after_remove_result = router.dispatch(post_request);
    expect_equal(after_remove_result.status, RouteStatus::NotFound, "path should be removed when no methods remain");

    expect_true(!router.del_route(HttpMethod::Post, "/echo"), "del missing route should fail");
}

void test_add_route_and_dispatch_concurrent() {
    nebula::http::Router router;
    constexpr int writer_iterations = 512;
    constexpr int reader_count = 4;
    constexpr int reader_iterations = 4096;

    std::atomic<bool> start = false;
    std::atomic<bool> writer_done = false;

    std::thread writer([&router, &start, &writer_done]() {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        for (int idx = 0; idx < writer_iterations; ++idx) {
            expect_true(router.add_route(HttpMethod::Get, "/dynamic/" + std::to_string(idx),
                                         [](const HttpRequest&) {
                                             return nebula::http::make_plain_text_response(HttpStatus::OK, "ok");
                                         }),
                        "add unique route in concurrent test should succeed");

            if (idx == 0) {
                expect_true(router.add_route(HttpMethod::Get, "/dynamic",
                                             [value = std::to_string(idx)](const HttpRequest&) {
                                                 return nebula::http::make_plain_text_response(HttpStatus::OK, value);
                                             }),
                            "first add dynamic route should succeed");
                continue;
            }

            expect_true(router.mod_route(HttpMethod::Get, "/dynamic",
                                         [value = std::to_string(idx)](const HttpRequest&) {
                                             return nebula::http::make_plain_text_response(HttpStatus::OK, value);
                                         }),
                        "mod dynamic route should succeed");
        }

        writer_done.store(true, std::memory_order_release);
    });

    std::vector<std::thread> readers;
    readers.reserve(reader_count);
    for (int idx = 0; idx < reader_count; ++idx) {
        readers.emplace_back([&router, &start, &writer_done]() {
            HttpRequest request;
            request.method = HttpMethod::Get;
            request.path = "/dynamic";

            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (int iter = 0; iter < reader_iterations; ++iter) {
                const auto result = router.dispatch(request);
                expect_true(result.status == RouteStatus::NotFound || result.status == RouteStatus::Matched,
                            "concurrent dispatch should return not found or matched");
            }

            while (!writer_done.load(std::memory_order_acquire)) {
                const auto result = router.dispatch(request);
                expect_true(result.status == RouteStatus::NotFound || result.status == RouteStatus::Matched,
                            "concurrent dispatch should return not found or matched");
            }
        });
    }

    start.store(true, std::memory_order_release);
    writer.join();
    for (std::thread& reader : readers) {
        reader.join();
    }

    HttpRequest final_request;
    final_request.method = HttpMethod::Get;
    final_request.path = "/dynamic";

    const auto final_result = router.dispatch(final_request);
    expect_equal(final_result.status, RouteStatus::Matched, "dynamic route should match after writer completes");
    expect_equal(final_result.response.status, HttpStatus::OK, "dynamic route should return 200");
    expect_equal(final_result.response.body, std::to_string(writer_iterations - 1),
                 "dynamic route should expose latest handler");
}

int run_router_tests() {
    const std::vector<nebula::testsupport::TestCase> tests = {
        {"route match", test_route_match},
        {"route not found", test_route_not_found},
        {"method not allowed", test_method_not_allowed},
        {"add route no duplicate", test_add_route_no_duplicate},
        {"mod route", test_mod_route},
        {"add route reject empty handler", test_add_route_reject_empty_handler},
        {"mod route reject empty handler", test_mod_route_reject_empty_handler},
        {"reject empty path", test_reject_empty_path},
        {"del route", test_del_route},
        {"add route and dispatch concurrent", test_add_route_and_dispatch_concurrent},
    };

    return nebula::testsupport::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::testsupport::run_main(run_router_tests);
}
