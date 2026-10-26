#include "scheduler/thread_pool.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using clock_type = std::chrono::steady_clock;

struct Measurement {
    double milliseconds;
    std::uint64_t checksum;
};

std::uint64_t workload(std::uint64_t seed, std::size_t iterations) {
    std::uint64_t value = seed + 0x9e3779b97f4a7c15ULL;
    for (std::size_t i = 0; i < iterations; ++i) {
        value ^= value >> 12;
        value ^= value << 25;
        value ^= value >> 27;
        value *= 0x2545F4914F6CDD1DULL;
    }
    return value;
}

Measurement run_sequential(std::size_t task_count, std::size_t iterations) {
    std::uint64_t checksum = 0;
    const auto start = clock_type::now();
    for (std::size_t i = 0; i < task_count; ++i) {
        checksum += workload(i, iterations);
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(clock_type::now() - start);
    return {elapsed.count(), checksum};
}

Measurement run_pool(std::size_t task_count, std::size_t iterations, std::size_t workers) {
    scheduler::ThreadPool pool(workers);
    std::vector<std::future<std::uint64_t>> futures;
    futures.reserve(task_count);

    const auto start = clock_type::now();
    for (std::size_t i = 0; i < task_count; ++i) {
        futures.emplace_back(pool.submit([i, iterations] { return workload(i, iterations); }));
    }

    std::uint64_t checksum = 0;
    for (auto& future : futures) {
        checksum += future.get();
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(clock_type::now() - start);
    return {elapsed.count(), checksum};
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

std::size_t parse_positive(const char* text, const char* label) {
    if (text[0] == '-') {
        throw std::invalid_argument(std::string(label) + " must be a positive integer");
    }
    std::size_t parsed_characters = 0;
    const auto value = std::stoull(text, &parsed_characters);
    if (text[parsed_characters] != '\0' || value == 0 ||
        value > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
        throw std::invalid_argument(std::string(label) + " must be a positive integer within size_t range");
    }
    return static_cast<std::size_t>(value);
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 4) {
            throw std::invalid_argument("usage: task_scheduler_benchmark [tasks] [iterations] [workers]");
        }

        const std::size_t tasks = argc > 1 ? parse_positive(argv[1], "tasks") : 20000;
        const std::size_t iterations = argc > 2 ? parse_positive(argv[2], "iterations") : 250;
        const std::size_t default_workers = std::max(1u, std::thread::hardware_concurrency());
        const std::size_t workers = argc > 3 ? parse_positive(argv[3], "workers") : default_workers;
        constexpr std::size_t repetitions = 5;

        // One unreported pass warms code and runtime paths for both variants.
        const auto warmup_sequential = run_sequential(std::min<std::size_t>(tasks, 1000), iterations);
        const auto warmup_pool = run_pool(std::min<std::size_t>(tasks, 1000), iterations, workers);
        if (warmup_sequential.checksum != warmup_pool.checksum) {
            throw std::runtime_error("warmup variants produced different checksums");
        }

        std::vector<double> sequential_times;
        std::vector<double> pool_times;
        std::uint64_t expected_checksum = 0;
        for (std::size_t run = 0; run < repetitions; ++run) {
            const auto sequential = run_sequential(tasks, iterations);
            const auto parallel = run_pool(tasks, iterations, workers);
            if (sequential.checksum != parallel.checksum) {
                throw std::runtime_error("benchmark variants produced different checksums");
            }
            expected_checksum = sequential.checksum;
            sequential_times.push_back(sequential.milliseconds);
            pool_times.push_back(parallel.milliseconds);
        }

        const double sequential_ms = median(sequential_times);
        const double pool_ms = median(pool_times);
        const double speedup = sequential_ms / pool_ms;

        std::cout << "Concurrent Task Scheduler benchmark (median of " << repetitions << " runs)\n"
                  << "Tasks: " << tasks << ", work iterations per task: " << iterations << '\n'
                  << "Workers: " << workers << " (hardware concurrency reported: "
                  << std::thread::hardware_concurrency() << ")\n"
                  << "Sequential baseline: " << sequential_ms << " ms\n"
                  << "Thread pool:         " << pool_ms << " ms\n"
                  << "Speedup:             " << speedup << "x\n"
                  << "Checksum:            " << expected_checksum << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "benchmark error: " << error.what() << '\n';
        return 1;
    }
}
