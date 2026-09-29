#include "thread_pool.h"

#include <iostream>
#include <stdexcept>
#include <utility>

namespace kv {

ThreadPool::ThreadPool(std::size_t worker_count, std::size_t max_queue_size)
    : worker_count_(worker_count), max_queue_size_(max_queue_size) {
    if (worker_count_ == 0 || max_queue_size_ == 0) {
        throw std::invalid_argument("thread pool sizes must be greater than zero");
    }
    workers_.reserve(worker_count_);
    for (std::size_t index = 0; index < worker_count_; ++index) {
        workers_.emplace_back(&ThreadPool::worker_loop, this);
    }
}

ThreadPool::~ThreadPool() { shutdown(); }

bool ThreadPool::submit(std::function<void()> task) {
    if (!task) return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || tasks_.size() >= max_queue_size_) return false;
        tasks_.push(std::move(task));
    }
    condition_.notify_one();
    return true;
}

void ThreadPool::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ && workers_.empty()) return;
        stopping_ = true;
    }
    condition_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
    workers_.clear();
}

std::size_t ThreadPool::worker_count() const noexcept { return worker_count_; }

void ThreadPool::worker_loop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
            if (tasks_.empty() && stopping_) return;
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        try {
            task();
        } catch (const std::exception& error) {
            std::cerr << "[ERROR] Worker task failed: " << error.what() << '\n';
        } catch (...) {
            std::cerr << "[ERROR] Worker task failed with an unknown exception\n";
        }
    }
}

}  // namespace kv
