#include "scheduler/thread_pool.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_results_and_argument_forwarding() {
    scheduler::ThreadPool pool(3);

    auto sum = pool.submit([](int left, int right) { return left + right; }, 19, 23);
    require(sum.get() == 42, "submit should return the callable result");

    int referenced_value = 4;
    auto reference_task = pool.submit([](int& value) { ++value; }, std::ref(referenced_value));
    reference_task.get();
    require(referenced_value == 5, "std::ref arguments should preserve references");

    auto move_only = pool.submit([owned = std::make_unique<int>(17)] { return *owned; });
    require(move_only.get() == 17, "submit should accept move-only callables");

    auto move_only_argument = pool.submit(
        [](std::unique_ptr<int> value) { return *value; }, std::make_unique<int>(29));
    require(move_only_argument.get() == 29, "submit should move move-only arguments into tasks");

    auto void_task = pool.submit([] {});
    void_task.get();
    pool.wait_idle();
}

void test_exception_propagation() {
    scheduler::ThreadPool pool(2);
    auto result = pool.submit([]() -> int {
        throw std::runtime_error("task failure");
    });

    bool propagated = false;
    try {
        (void)result.get();
    } catch (const std::runtime_error& error) {
        propagated = std::string(error.what()) == "task failure";
    }
    require(propagated, "task exceptions should be rethrown by future::get");

    auto still_works = pool.submit([] { return 7; });
    require(still_works.get() == 7, "a task exception must not stop a worker");
}

void test_invalid_worker_count() {
    bool rejected = false;
    try {
        scheduler::ThreadPool pool(0);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "zero workers should be rejected");
}

void test_concurrent_shutdown_and_rejection() {
    scheduler::ThreadPool pool(1);
    std::promise<void> release_promise;
    const auto release = release_promise.get_future().share();

    auto running = pool.submit([release] { release.wait(); });
    auto queued = pool.submit([] { return 73; });

    std::vector<std::future<void>> shutdown_calls;
    for (int i = 0; i < 4; ++i) {
        shutdown_calls.emplace_back(std::async(std::launch::async, [&pool] { pool.shutdown(); }));
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (pool.is_accepting() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }

    std::future<int> raced_submission;
    bool rejected = false;
    try {
        raced_submission = pool.submit([] { return 1; });
    } catch (const std::runtime_error&) {
        rejected = true;
    }

    release_promise.set_value();
    running.get();
    require(queued.get() == 73, "shutdown should drain previously accepted tasks");
    for (auto& call : shutdown_calls) {
        call.get();
    }
    if (raced_submission.valid()) {
        (void)raced_submission.get();
    }

    require(!pool.is_accepting(), "shutdown should permanently close submissions");
    require(rejected, "submissions after shutdown begins should be rejected");

    bool rejected_after_shutdown = false;
    try {
        (void)pool.submit([] { return 2; });
    } catch (const std::runtime_error&) {
        rejected_after_shutdown = true;
    }
    require(rejected_after_shutdown, "submissions after shutdown completes should be rejected");
    pool.shutdown(); // repeated shutdown is an idempotent no-op
}

void test_worker_cannot_join_its_own_pool() {
    scheduler::ThreadPool pool(1);
    auto self_shutdown = pool.submit([&pool] { pool.shutdown(); });

    bool rejected = false;
    try {
        self_shutdown.get();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    require(rejected, "worker-initiated shutdown should fail instead of self-joining");
    require(pool.is_accepting(), "rejected worker shutdown should not partially close the pool");
    require(pool.submit([] { return 9; }).get() == 9,
            "pool should remain usable after a worker shutdown attempt is rejected");
    pool.shutdown();
}

void test_submit_racing_shutdown() {
    scheduler::ThreadPool pool(3);
    std::atomic<std::size_t> completed{0};
    std::promise<void> first_submission_promise;
    auto first_submission = first_submission_promise.get_future();
    std::vector<std::future<void>> accepted;

    std::thread producer([&] {
        for (std::size_t i = 0; i < 2000; ++i) {
            try {
                auto future = pool.submit([&completed] {
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                    completed.fetch_add(1, std::memory_order_relaxed);
                });
                accepted.emplace_back(std::move(future));
                if (i == 0) {
                    first_submission_promise.set_value();
                }
            } catch (const std::runtime_error&) {
                break;
            }
        }
    });

    first_submission.wait();
    pool.shutdown();
    producer.join();
    for (auto& future : accepted) {
        future.get();
    }
    require(completed.load(std::memory_order_relaxed) == accepted.size(),
            "every task accepted before shutdown must complete exactly once");
}

void test_multi_producer_stress() {
    constexpr int producer_count = 6;
    constexpr int tasks_per_producer = 500;
    scheduler::ThreadPool pool(4);
    std::atomic<int> total{0};
    std::vector<std::thread> producers;
    std::vector<std::vector<std::future<void>>> all_futures(producer_count);

    for (int producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([producer, &total, &pool, &all_futures] {
            auto& futures = all_futures[producer];
            futures.reserve(tasks_per_producer);
            for (int task = 0; task < tasks_per_producer; ++task) {
                futures.emplace_back(pool.submit([&total] {
                    total.fetch_add(1, std::memory_order_relaxed);
                }));
            }
        });
    }

    for (auto& producer : producers) {
        producer.join();
    }
    for (auto& futures : all_futures) {
        for (auto& future : futures) {
            future.get();
        }
    }
    pool.wait_idle();
    require(total.load(std::memory_order_relaxed) == producer_count * tasks_per_producer,
            "all tasks from concurrent producers should run exactly once");
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"results and argument forwarding", test_results_and_argument_forwarding},
        {"exception propagation", test_exception_propagation},
        {"invalid worker count", test_invalid_worker_count},
        {"concurrent shutdown and rejection", test_concurrent_shutdown_and_rejection},
        {"worker shutdown guard", test_worker_cannot_join_its_own_pool},
        {"submit racing shutdown", test_submit_racing_shutdown},
        {"multi-producer stress", test_multi_producer_stress},
    };

    int failures = 0;
    for (const auto& test : tests) {
        try {
            test.second();
            std::cout << "[PASS] " << test.first << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << test.first << ": " << error.what() << '\n';
        }
    }

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All " << tests.size() << " tests passed\n";
    return 0;
}
