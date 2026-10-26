# Concurrent Task Scheduler

A small, dependency-free C++17 thread pool focused on the lifecycle and synchronization details that matter in a database or server process. It uses a fixed set of workers, a shared FIFO task queue, and futures for results and exceptions.

The project demonstrates practical concurrency fundamentals relevant to a SingleStore Software Engineer interview: synchronization design, task lifecycle guarantees, contention trade-offs, exception safety, and measurement discipline. It is intentionally small enough to explain end to end.

## Features

- Fixed-size worker pool; a zero-worker pool is rejected.
- Thread-safe submission from multiple caller threads.
- Templated `submit` supports return values, `void`, forwarded arguments, `std::ref`, and move-only callables. It returns `std::future<T>`.
- Condition-variable wakeups; workers sleep while the queue is empty.
- Exceptions from callables are delivered through `future::get()` and do not terminate worker threads.
- Graceful, idempotent shutdown: accepted tasks finish, new submissions fail synchronously with `std::runtime_error`, and workers are joined.
- `wait_idle()` for waiting until the queue and active-task count are both empty at one observed instant.
- Multi-producer, shutdown-race, exception, lifecycle, and task-result tests.
- A repeatable benchmark comparing the pool with a sequential baseline.
- GitHub Actions CI that builds and runs the tests on Linux and Windows.

## Build and run

Requirements: CMake 3.16 or newer and a C++17 compiler.

### Windows PowerShell

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
.\build\Release\task_scheduler_benchmark.exe 20000 250 4
```

For single-configuration generators such as MinGW Makefiles, the benchmark is at `build\task_scheduler_benchmark.exe`.

### Linux or macOS

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/task_scheduler_benchmark 20000 250 4
```

The benchmark arguments are `[task count] [work iterations per task] [worker count]`. Defaults are 20,000 tasks, 250 iterations, and the reported hardware concurrency (at least one worker). It performs one warmup and reports the median of five measured runs. The timer includes task submission, execution, and future collection; pool construction and destruction are outside the timed interval. The sequential and pool variants use identical deterministic work and compare checksums before reporting.

To build without the benchmark, configure with `-DSCHEDULER_BUILD_BENCHMARKS=OFF`. Tests are registered with CTest and can be disabled with `-DBUILD_TESTING=OFF`.

## Example

```cpp
#include "scheduler/thread_pool.hpp"

#include <iostream>

int main() {
    scheduler::ThreadPool pool(4);
    auto result = pool.submit([](int left, int right) { return left + right; }, 20, 22);
    std::cout << result.get() << '\n';
    pool.shutdown(); // optional; the destructor also shuts down and joins
}
```

## Architecture and synchronization

`ThreadPool` owns a `std::deque<std::function<void()>>`, its queue mutex, two condition variables, and the worker threads. Submission wraps the callable and arguments in `std::packaged_task`, obtains the future, then takes the queue mutex to check the lifecycle state and append the task. Releasing the mutex before notifying avoids waking a worker only to have it block immediately on the same lock.

Workers wait until either work is available or submissions are closed. They pop one task while holding the queue mutex, increment the active count, then execute without the lock. On completion they decrement the count and notify `wait_idle()` when both the queue and active count reach zero. User code therefore cannot hold the queue lock while it runs.

`shutdown()` is serialized by a separate mutex, while its state transition shares the queue mutex with `submit()`. This gives the submit/shutdown race a clear boundary: a submission that gets the queue lock first is accepted and drained; a shutdown that gets it first closes the pool and causes the submission to throw. Shutdown wakes all workers and joins them only after queued work has drained. Concurrent shutdown callers wait for the first caller to finish joining and then return safely.

## Design trade-offs and limitations

- The queue is unbounded. This keeps submission simple, but a producer can consume memory faster than workers can process tasks. A bounded queue needs an explicit blocking, rejection, or caller-runs policy and careful shutdown semantics.
- Tasks are FIFO when dequeued, with no priority, cancellation, work stealing, or fairness guarantee. Completion order can differ from submission order.
- Shutdown drains accepted work and does not cancel it. A task that never returns can keep shutdown waiting indefinitely.
- `wait_idle()` observes one instant. Another caller can submit immediately after it returns.
- Call `shutdown()` and destroy the pool from a thread outside its own workers. A worker calling `shutdown()` receives `std::logic_error`. Destroying the pool from one of its own tasks is unsupported because the worker loop still depends on the pool object's lifetime.
- A pool is not a substitute for task-level synchronization: callers must still protect shared state used by their submitted callables.
- The benchmark is a local microbenchmark, not a performance claim. Results vary with CPU, compiler, build mode, task size, and machine load. For tiny tasks, queue and future overhead can outweigh parallel work.

## Interview discussion points

- **Lifecycle linearization:** explain why submissions accepted before shutdown drain, and why later submissions fail.
- **Lock scope:** the queue lock covers queue/state changes but never user task execution; the shutdown mutex prevents concurrent callers from joining the same threads.
- **Exception safety:** `packaged_task` converts callable exceptions into future state, while the worker loop remains alive.
- **Backpressure:** discuss queue growth under overload and compare bounded blocking, rejection, and caller-runs policies.
- **Database-system extensions:** consider per-tenant queues, priorities, cancellation, task deadlines, metrics, work stealing, and NUMA-aware placement, then describe which new invariants each would require.
- **Benchmark discipline:** compare equivalent workloads, validate result checksums, report medians, and explain why results depend on task granularity and hardware.

## Repository layout

```text
include/scheduler/thread_pool.hpp   Public API and templated submit
src/thread_pool.cpp                 Worker lifecycle and synchronization
tests/thread_pool_tests.cpp         Dependency-free automated tests
benchmarks/benchmark.cpp            Sequential baseline comparison
CMakeLists.txt                      Build and CTest configuration
```

## License

MIT. See [LICENSE](LICENSE).
