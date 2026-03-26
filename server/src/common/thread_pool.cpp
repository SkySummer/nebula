#include "nebula/common/thread_pool.hpp"

namespace nebula::common {

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
}

ThreadPool::~ThreadPool() {
    stop();
}

void ThreadPool::stop() {
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
    }

    condition_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
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
