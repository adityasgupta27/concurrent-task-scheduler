#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace scheduler {

// A fixed-size pool with an unbounded FIFO queue. Accepted tasks are drained
// during shutdown. The pool must be shut down or destroyed from a non-worker
// thread; destroying an object from one of its own tasks cannot be made safe.
class ThreadPool {
public:
    explicit ThreadPool(std::size_t worker_count);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    // Submit is linearized with shutdown under the queue mutex. If shutdown
    // has started, it throws std::runtime_error; otherwise its future receives
    // either the result or the callable's exception.
    template <class F, class... Args>
    auto submit(F&& function, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<F>&, std::decay_t<Args>&&...>> {
        using result_type =
            std::invoke_result_t<std::decay_t<F>&, std::decay_t<Args>&&...>;
        using arguments_type = std::tuple<std::decay_t<Args>...>;

        auto invocation = [callable = std::decay_t<F>(std::forward<F>(function)),
                           arguments = arguments_type(std::forward<Args>(args)...)]() mutable
            -> result_type {
            return std::apply(
                [&callable](auto&&... unpacked) -> result_type {
                    return std::invoke(callable,
                                       std::forward<decltype(unpacked)>(unpacked)...);
                },
                std::move(arguments));
        };
        auto packaged_task = std::make_shared<std::packaged_task<result_type()>>(
            std::move(invocation));
        auto result = packaged_task->get_future();

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (!accepting_) {
                throw std::runtime_error("cannot submit to a thread pool that is shutting down");
            }
            tasks_.emplace_back([packaged_task] { (*packaged_task)(); });
        }
        work_available_.notify_one();
        return result;
    }

    // Stop accepting new work, drain all accepted work, and join every worker.
    // Safe to call repeatedly and concurrently from non-worker threads.
    void shutdown();

    // Wait until the queue and active task count are both zero at one instant.
    // A concurrent submit may enqueue new work immediately after this returns.
    void wait_idle();

    bool is_accepting() const;
    std::size_t worker_count() const noexcept;

private:
    void worker_loop();

    mutable std::mutex queue_mutex_;
    std::mutex shutdown_mutex_;
    std::condition_variable work_available_;
    std::condition_variable became_idle_;
    std::deque<std::function<void()>> tasks_;
    std::vector<std::thread> workers_;
    std::size_t active_workers_ = 0;
    bool accepting_ = true;
};

} // namespace scheduler
