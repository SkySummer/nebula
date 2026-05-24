#include "nebula/common/runtime/thread_pool.hpp"

namespace nebula::common {

ThreadPool::ThreadPool(std::size_t worker_count) {
    if (worker_count == 0U) {
        worker_count = 1U;
    }

    try {
        workers_.reserve(worker_count);
        for (std::size_t idx = 0; idx < worker_count; ++idx) {
            workers_.emplace_back([this](const std::stop_token& stop) { worker_loop(stop); });
        }
    } catch (...) {
        stop();
        throw;
    }
}

ThreadPool::~ThreadPool() noexcept {
    stop();
}

void ThreadPool::stop() noexcept {
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
    }

    condition_.notify_all();
    for (std::jthread& worker : workers_) {
        worker.request_stop();
    }
    workers_.clear();
}

void ThreadPool::worker_loop(const std::stop_token& stop) {
    while (true) {
        std::function<void()> task;

        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, stop, [this]() { return !tasks_.empty(); });
            if (stop.stop_requested() && tasks_.empty()) {
                return;
            }

            task = std::move(tasks_.front());
            tasks_.pop();
        }

        task();
    }
}

}  // namespace nebula::common
