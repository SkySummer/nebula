#include "nebula/server/runtime/http_request_dispatcher.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "nebula/common/runtime/thread_pool.hpp"
#include "nebula/http/routing/router.hpp"
#include "nebula_tests/common.hpp"

namespace {

using namespace nebula;

void test_dispatch_submits_internal_error_response_when_handler_throws() {
    auto router = std::make_shared<http::Router>();
    const bool added = router->add_route(
        http::HttpMethod::Get, "/boom",
        [](const nebula::http::RouteContext&) -> http::HttpResponse { throw std::runtime_error("handler_failed"); });
    test::expect_true(added, "route should be registered");

    common::ThreadPool thread_pool(1);
    std::promise<server::ReactorResponseTask> submitted_promise;
    std::future<server::ReactorResponseTask> submitted_future = submitted_promise.get_future();
    server::HttpRequestDispatcher dispatcher(
        router, nullptr, thread_pool,
        [&submitted_promise](server::ReactorResponseTask task) { submitted_promise.set_value(std::move(task)); });

    dispatcher.dispatch(server::ReactorRequestTask{
        .reactor_id = 3,
        .fd = 42,
        .connection_token = 99,
        .request =
            http::HttpRequest{
                .method = http::HttpMethod::Get,
                .path = "/boom",
                .query_params = {},
                .request_line = "GET /boom HTTP/1.1",
                .headers = {},
                .body = {},
                .keep_alive = true,
            },
        .close_after_write = false,
        .suppress_body = false,
        .request_line = "GET /boom HTTP/1.1",
        .request_bytes = 128,
        .request_started_at = std::chrono::steady_clock::now(),
    });

    server::ReactorResponseTask submitted = submitted_future.get();
    thread_pool.stop();

    test::expect_equal(submitted.reactor_id, std::size_t{3}, "reactor id should be preserved");
    test::expect_equal(submitted.fd, 42, "fd should be preserved");
    test::expect_equal(submitted.connection_token, std::uint64_t{99}, "connection token should be preserved");
    test::expect_equal(submitted.response.status, http::HttpStatus::InternalServerError,
                       "handler exception should return internal server error");
    test::expect_true(submitted.close_after_write, "handler exception should force close after write");
    test::expect_true(!submitted.suppress_body, "suppress body should be preserved");
    test::expect_equal(submitted.request_line, std::string("GET /boom HTTP/1.1"), "request line should be preserved");
    test::expect_equal(submitted.request_bytes, std::size_t{128}, "request bytes should be preserved");
}

void test_dispatch_does_not_resubmit_error_response_when_submitter_throws() {
    auto router = std::make_shared<http::Router>();
    const bool added =
        router->add_route(http::HttpMethod::Get, "/ok", [](const nebula::http::RouteContext&) -> http::HttpResponse {
            return http::HttpResponse{
                .status = http::HttpStatus::OK,
                .headers = {},
                .body = "ok",
            };
        });
    test::expect_true(added, "route should be registered");

    common::ThreadPool thread_pool(1);
    std::promise<void> attempted_submit_promise;
    std::future<void> attempted_submit_future = attempted_submit_promise.get_future();
    std::atomic<int> submit_count = 0;
    server::HttpRequestDispatcher dispatcher(
        router, nullptr, thread_pool, [&submit_count, &attempted_submit_promise](const server::ReactorResponseTask&) {
            submit_count.fetch_add(1);
            attempted_submit_promise.set_value();
            throw std::runtime_error("submit_failed");
        });

    dispatcher.dispatch(server::ReactorRequestTask{
        .reactor_id = 1,
        .fd = 7,
        .connection_token = 11,
        .request =
            http::HttpRequest{
                .method = http::HttpMethod::Get,
                .path = "/ok",
                .query_params = {},
                .request_line = "GET /ok HTTP/1.1",
                .headers = {},
                .body = {},
                .keep_alive = true,
            },
        .close_after_write = false,
        .suppress_body = false,
        .request_line = "GET /ok HTTP/1.1",
        .request_bytes = 64,
        .request_started_at = std::chrono::steady_clock::now(),
    });

    attempted_submit_future.get();
    thread_pool.stop();

    test::expect_equal(submit_count.load(), 1, "submitter exception should not trigger a second error submission");
}

int run_http_request_dispatcher_tests() {
    std::vector<nebula::test::TestCase> tests = {
        {"dispatch submits internal error response when handler throws",
         test_dispatch_submits_internal_error_response_when_handler_throws},
        {"dispatch does not resubmit error response when submitter throws",
         test_dispatch_does_not_resubmit_error_response_when_submitter_throws},
    };

    return nebula::test::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::test::run_main(run_http_request_dispatcher_tests);
}
