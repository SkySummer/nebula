#include "nebula/common/thread_pool.hpp"

#include "nebula/common/logger.hpp"

namespace nebula::common {

ThreadPool::ThreadPool(std::size_t worker_count) {
    if (worker_count == 0U) {
        worker_count = 1U;
    }

    try {
        workers_.reserve(worker_count);
        for (std::size_t idx = 0; idx < worker_count; ++idx) {
            workers_.emplace_back(&ThreadPool::worker_loop, this);
        }
    } catch (...) {
        stop();
        throw;
    }

    Logger::instance().info(LogDomain::Common, "thread pool started").field("count", workers_.size());
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

    Logger::instance().info(LogDomain::Common, "thread pool stopped").field("count", worker_count);
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
