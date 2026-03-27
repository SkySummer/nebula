#include "nebula/common/thread_pool.hpp"

#include <cstdio>

#include "nebula/common/logger.hpp"

namespace nebula::common {

namespace {

void report_log_emit_error(const char* event) noexcept {
    std::fputs("thread pool log emit failed: event=", stderr);
    std::fputs(event != nullptr ? event : "unknown", stderr);
    std::fputs(", error=logger_emit_failed, decision=ignore", stderr);
    std::fputc('\n', stderr);
}

}  // namespace

ThreadPool::ThreadPool(std::size_t worker_count) {
    if (worker_count == 0U) {
        worker_count = 1U;
    }

    try {
        workers_.reserve(worker_count);
        for (std::size_t idx = 0; idx < worker_count; ++idx) {
            workers_.emplace_back([this]() { worker_loop(); });
        }
    } catch (...) {
        stop();
        throw;
    }

    try {
        Logger::instance().info("thread pool started").field("count", workers_.size());
    } catch (...) {
        report_log_emit_error("thread_pool_started");
    }
}

ThreadPool::~ThreadPool() noexcept {
    stop();
}

void ThreadPool::stop() noexcept {
    std::size_t worker_count = 0;
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
        worker_count = workers_.size();
    }

    condition_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();

    try {
        Logger::instance().info("thread pool stopped").field("count", worker_count);
    } catch (...) {
        report_log_emit_error("thread_pool_stopped");
    }
}

void ThreadPool::worker_loop() {
    while (true) {
        std::function<void()> task;

        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this]() { return stopping_ || !tasks_.empty(); });
            if (stopping_ && tasks_.empty()) {
                return;
            }

            task = std::move(tasks_.front());
            tasks_.pop();
        }

        task();
    }
}

}  // namespace nebula::common
