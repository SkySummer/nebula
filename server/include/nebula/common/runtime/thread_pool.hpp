#ifndef NEBULA_COMMON_RUNTIME_THREAD_POOL_HPP
#define NEBULA_COMMON_RUNTIME_THREAD_POOL_HPP

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace nebula::common {

class ThreadPool {
public:
    explicit ThreadPool(std::size_t worker_count);
    ~ThreadPool() noexcept;

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    template <typename Fn>
    auto submit(Fn&& fn) -> std::future<std::invoke_result_t<Fn>> {
        auto task = std::make_shared<std::packaged_task<std::invoke_result_t<Fn>()>>(std::forward<Fn>(fn));
        std::future<std::invoke_result_t<Fn>> future = task->get_future();

        {
            std::lock_guard lock(mutex_);
            if (stopping_) {
                throw std::runtime_error("thread pool is stopping");
            }
            tasks_.push([task]() { (*task)(); });
        }
        condition_.notify_one();
        return future;
    }

    void stop() noexcept;

private:
    void worker_loop(const std::stop_token& stop);

    std::mutex mutex_;
    std::condition_variable_any condition_;
    std::queue<std::function<void()>> tasks_;
    std::vector<std::jthread> workers_;
    bool stopping_ = false;
};

}  // namespace nebula::common

#endif  // NEBULA_COMMON_RUNTIME_THREAD_POOL_HPP
