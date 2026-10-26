cmake_minimum_required(VERSION 3.16)
project(concurrent_task_scheduler VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(task_scheduler
    src/thread_pool.cpp
)
target_include_directories(task_scheduler PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(task_scheduler PUBLIC Threads::Threads)

if(MSVC)
    target_compile_options(task_scheduler PRIVATE /W4 /permissive-)
else()
    target_compile_options(task_scheduler PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(task_scheduler_tests tests/thread_pool_tests.cpp)
    target_link_libraries(task_scheduler_tests PRIVATE task_scheduler)
    if(MSVC)
        target_compile_options(task_scheduler_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(task_scheduler_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME task_scheduler_tests COMMAND task_scheduler_tests)
endif()

option(SCHEDULER_BUILD_BENCHMARKS "Build the standalone scheduler benchmark" ON)
if(SCHEDULER_BUILD_BENCHMARKS)
    add_executable(task_scheduler_benchmark benchmarks/benchmark.cpp)
    target_link_libraries(task_scheduler_benchmark PRIVATE task_scheduler)
    if(MSVC)
        target_compile_options(task_scheduler_benchmark PRIVATE /W4 /permissive-)
    else()
        target_compile_options(task_scheduler_benchmark PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()
