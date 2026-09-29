#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace kv {

class ThreadPool {
public:
    ThreadPool(std::size_t worker_count, std::size_t max_queue_size);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    bool submit(std::function<void()> task);
    void shutdown();
    std::size_t worker_count() const noexcept;

private:
    void worker_loop();

    const std::size_t worker_count_;
    const std::size_t max_queue_size_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<std::function<void()>> tasks_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
};

}  // namespace kv
