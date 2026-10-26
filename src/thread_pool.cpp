#include "scheduler/thread_pool.hpp"

#include <stdexcept>

namespace scheduler {

namespace {

thread_local const ThreadPool* current_worker_pool = nullptr;

} // namespace

ThreadPool::ThreadPool(std::size_t worker_count) {
    if (worker_count == 0) {
        throw std::invalid_argument("a thread pool must have at least one worker");
    }

    workers_.reserve(worker_count);
    try {
        for (std::size_t i = 0; i < worker_count; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            accepting_ = false;
        }
        work_available_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        throw;
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::shutdown() {
    if (current_worker_pool == this) {
        throw std::logic_error("shutdown must be called from outside the thread pool");
    }

    std::lock_guard<std::mutex> shutdown_lock(shutdown_mutex_);

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        accepting_ = false;
    }
    work_available_.notify_all();

    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void ThreadPool::wait_idle() {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    became_idle_.wait(lock, [this] {
        return tasks_.empty() && active_workers_ == 0;
    });
}

bool ThreadPool::is_accepting() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return accepting_;
}

std::size_t ThreadPool::worker_count() const noexcept {
    return workers_.size();
}

void ThreadPool::worker_loop() {
    current_worker_pool = this;
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            work_available_.wait(lock, [this] {
                return !tasks_.empty() || !accepting_;
            });

            if (tasks_.empty()) {
                // Empty queue plus closed submissions means graceful exit.
                current_worker_pool = nullptr;
                return;
            }

            task = std::move(tasks_.front());
            tasks_.pop_front();
            ++active_workers_;
        }

        // packaged_task transports user exceptions to its future. This catch
        // also ensures an unexpected wrapper exception cannot kill a worker.
        try {
            task();
        } catch (...) {
        }

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            --active_workers_;
            if (tasks_.empty() && active_workers_ == 0) {
                became_idle_.notify_all();
            }
        }
    }
}

} // namespace scheduler
