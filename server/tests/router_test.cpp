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

nebula::http::Router::Handler plain_text_handler(std::string text) {
    return [body = std::move(text)](const nebula::http::RouteContext&) {
        return nebula::http::make_plain_text_response(HttpStatus::OK, body);
    };
}

bool is_concurrent_dispatch_status_allowed(RouteStatus status) {
    return status == RouteStatus::NotFound || status == RouteStatus::Matched;
}

void run_concurrent_writer(nebula::http::Router& router, int writer_iterations, std::atomic<bool>& start,
                           std::atomic<bool>& writer_done, std::atomic<bool>& failed) {
    while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    for (int idx = 0; idx < writer_iterations; ++idx) {
        if (!router.add_route(HttpMethod::Get, "/dynamic/" + std::to_string(idx), plain_text_handler("ok"))) {
            failed.store(true, std::memory_order_release);
            return;
        }

        if (idx == 0) {
            if (!router.add_route(HttpMethod::Get, "/dynamic", plain_text_handler(std::to_string(idx)))) {
                failed.store(true, std::memory_order_release);
                return;
            }
            continue;
        }

        if (!router.mod_route(HttpMethod::Get, "/dynamic", plain_text_handler(std::to_string(idx)))) {
            failed.store(true, std::memory_order_release);
            return;
        }
    }

    writer_done.store(true, std::memory_order_release);
}

void run_concurrent_reader(const nebula::http::Router& router, int reader_iterations, std::atomic<bool>& start,
                           std::atomic<bool>& writer_done, std::atomic<bool>& failed) {
    HttpRequest request;
    request.method = HttpMethod::Get;
    request.path = "/dynamic";

    while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    for (int iter = 0; iter < reader_iterations; ++iter) {
        const auto result = router.dispatch(request);
        if (!is_concurrent_dispatch_status_allowed(result.status)) {
            failed.store(true, std::memory_order_release);
            return;
        }
    }

    while (!writer_done.load(std::memory_order_acquire)) {
        const auto result = router.dispatch(request);
        if (!is_concurrent_dispatch_status_allowed(result.status)) {
            failed.store(true, std::memory_order_release);
            return;
        }
    }
}

void test_route_match() {
    nebula::http::Router router;
    expect_true(router.add_route(HttpMethod::Get, "/healthz", plain_text_handler("ok")), "add route should succeed");

    HttpRequest request;
    request.method = HttpMethod::Get;
    request.path = "/healthz";

    const auto result = router.dispatch(request);
    expect_equal(result.status, RouteStatus::Matched, "route should match");
    expect_equal(result.response.status, HttpStatus::OK, "status should be 200");
}

void test_route_not_found() {
    nebula::http::Router router;
    expect_true(router.add_route(HttpMethod::Get, "/healthz", plain_text_handler("ok")), "add route should succeed");

    HttpRequest request;
    request.method = HttpMethod::Get;
    request.path = "/unknown";

    const auto result = router.dispatch(request);
    expect_equal(result.status, RouteStatus::NotFound, "path should not match");
}

void test_method_not_allowed() {
    nebula::http::Router router;
    expect_true(router.add_route(HttpMethod::Get, "/echo", plain_text_handler("ok")), "add route should succeed");

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
    expect_true(router.add_route(HttpMethod::Get, "/echo", plain_text_handler("v1")), "first add should succeed");
    expect_true(!router.add_route(HttpMethod::Get, "/echo", plain_text_handler("v2")), "duplicate add should fail");

    HttpRequest request;
    request.method = HttpMethod::Get;
    request.path = "/echo";
    const auto result = router.dispatch(request);
    expect_equal(result.status, RouteStatus::Matched, "duplicate add failure should keep original route");
    expect_equal(result.response.body, std::string("v1"), "duplicate add should not overwrite original handler");
}

void test_mod_route() {
    nebula::http::Router router;
    expect_true(router.add_route(HttpMethod::Get, "/echo", plain_text_handler("before")), "add route should succeed");
    expect_true(router.mod_route(HttpMethod::Get, "/echo", plain_text_handler("after")),
                "mod existing route should succeed");
    expect_true(!router.mod_route(HttpMethod::Post, "/echo", plain_text_handler("nope")),
                "mod missing method should fail");
    expect_true(!router.mod_route(HttpMethod::Get, "/missing", plain_text_handler("nope")),
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
    expect_true(router.add_route(HttpMethod::Get, "/echo", plain_text_handler("keep")), "add route should succeed");

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

    expect_true(!router.add_route(HttpMethod::Get, "", plain_text_handler("ok")),
                "add route with empty path should fail");
    expect_true(!router.mod_route(HttpMethod::Get, "", plain_text_handler("ok")),
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
    expect_true(router.add_route(HttpMethod::Get, "/echo", plain_text_handler("get")), "add get route should succeed");
    expect_true(router.add_route(HttpMethod::Post, "/echo", plain_text_handler("post")),
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

void test_dynamic_route_match_with_param() {
    nebula::http::Router router;
    expect_true(router.add_route(HttpMethod::Get, "/users/{id}",
                                 [](const nebula::http::RouteContext& context) {
                                     return nebula::http::make_plain_text_response(HttpStatus::OK,
                                                                                   context.params.at("id"));
                                 }),
                "add dynamic route should succeed");

    HttpRequest request;
    request.method = HttpMethod::Get;
    request.path = "/users/42";

    const auto result = router.dispatch(request);
    expect_equal(result.status, RouteStatus::Matched, "dynamic route should match");
    expect_equal(result.response.body, std::string("42"), "dynamic param should be passed to handler");
}

void test_dynamic_route_reject_empty_param_segment() {
    nebula::http::Router router;
    expect_true(router.add_route(HttpMethod::Get, "/users/{id}", plain_text_handler("dynamic")),
                "add dynamic route should succeed");

    HttpRequest request;
    request.method = HttpMethod::Get;
    request.path = "/users/";

    const auto result = router.dispatch(request);
    expect_equal(result.status, RouteStatus::NotFound, "dynamic route should reject empty param segment");
    expect_true(!router.has_route_match(HttpMethod::Get, "/users/"), "match query should reject empty dynamic segment");
}

void test_static_route_preferred_over_dynamic_route() {
    nebula::http::Router router;
    expect_true(router.add_route(HttpMethod::Get, "/users/{id}", plain_text_handler("dynamic")),
                "add dynamic route should succeed");
    expect_true(router.add_route(HttpMethod::Get, "/users/me", plain_text_handler("static")),
                "add static route should succeed");

    HttpRequest request;
    request.method = HttpMethod::Get;
    request.path = "/users/me";
    const auto result = router.dispatch(request);

    expect_equal(result.status, RouteStatus::Matched, "request should match static route");
    expect_equal(result.response.body, std::string("static"), "static route should win on same level");
}

void test_reject_ambiguous_dynamic_route() {
    nebula::http::Router router;
    expect_true(router.add_route(HttpMethod::Get, "/users/{id}", plain_text_handler("id")),
                "first dynamic route should succeed");
    expect_true(!router.add_route(HttpMethod::Get, "/users/{name}", plain_text_handler("name")),
                "dynamic route with different param name on same level should fail");
}

void test_dynamic_route_mod_and_del() {
    nebula::http::Router router;
    expect_true(router.add_route(HttpMethod::Get, "/books/{id}", plain_text_handler("before")),
                "add dynamic route should succeed");

    expect_true(router.mod_route(HttpMethod::Get, "/books/{id}", plain_text_handler("after")),
                "mod dynamic route should succeed");

    HttpRequest request;
    request.method = HttpMethod::Get;
    request.path = "/books/7";
    const auto after_mod = router.dispatch(request);
    expect_equal(after_mod.status, RouteStatus::Matched, "modded dynamic route should still match");
    expect_equal(after_mod.response.body, std::string("after"), "mod should replace dynamic handler");

    expect_true(router.del_route(HttpMethod::Get, "/books/{id}"), "del dynamic route should succeed");
    const auto after_del = router.dispatch(request);
    expect_equal(after_del.status, RouteStatus::NotFound, "deleted dynamic route should not match");
}

void test_has_route_query_semantics() {
    nebula::http::Router router;
    expect_true(router.add_route(HttpMethod::Get, "/users/{id}", plain_text_handler("dynamic-get")),
                "add dynamic get route should succeed");
    expect_true(router.add_route(HttpMethod::Post, "/users/{id}", plain_text_handler("dynamic-post")),
                "add dynamic post route should succeed");
    expect_true(router.add_route(HttpMethod::Get, "/users/me", plain_text_handler("static-get")),
                "add static route should succeed");

    expect_true(router.has_route_exact(HttpMethod::Get, "/users/{id}"), "exact dynamic path should be found");
    expect_true(!router.has_route_exact(HttpMethod::Get, "/users/42"),
                "exact query should not match concrete request path");

    expect_true(router.has_route_match(HttpMethod::Get, "/users/42"), "match query should match dynamic route");
    expect_true(!router.has_route_match(HttpMethod::Put, "/users/42"), "missing method should not match");

    expect_true(!router.has_route_match(HttpMethod::Post, "/users/me"),
                "static path should take priority over dynamic path when checking method match");
}

void test_add_route_and_dispatch_concurrent() {
    nebula::http::Router router;
    constexpr int writer_iterations = 512;
    constexpr int reader_count = 4;
    constexpr int reader_iterations = 4096;

    std::atomic<bool> start = false;
    std::atomic<bool> writer_done = false;
    std::atomic<bool> failed = false;

    std::thread writer([&router, &start, &writer_done, &failed]() {
        run_concurrent_writer(router, writer_iterations, start, writer_done, failed);
    });

    std::vector<std::thread> readers;
    readers.reserve(reader_count);
    for (int idx = 0; idx < reader_count; ++idx) {
        readers.emplace_back([&router, &start, &writer_done, &failed]() {
            run_concurrent_reader(router, reader_iterations, start, writer_done, failed);
        });
    }

    start.store(true, std::memory_order_release);
    writer.join();
    for (std::thread& reader : readers) {
        reader.join();
    }
    expect_true(!failed.load(std::memory_order_acquire), "concurrent add/mod/dispatch should stay consistent");

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
        {"dynamic route match with param", test_dynamic_route_match_with_param},
        {"dynamic route reject empty param segment", test_dynamic_route_reject_empty_param_segment},
        {"static route preferred over dynamic route", test_static_route_preferred_over_dynamic_route},
        {"reject ambiguous dynamic route", test_reject_ambiguous_dynamic_route},
        {"dynamic route mod and del", test_dynamic_route_mod_and_del},
        {"has route query semantics", test_has_route_query_semantics},
        {"add route and dispatch concurrent", test_add_route_and_dispatch_concurrent},
    };

    return nebula::testsupport::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::testsupport::run_main(run_router_tests);
}
