// =============================================================================
//  file : include/librazobill/ThreadPool.hpp
//  role : cache-aware worker thread pool for chunked parallel execution
//  inv  : coarse chunking amortizes queue mutex; joins all workers on shutdown
// =============================================================================

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace razobill::core {

// -- worker thread pool -------------------------------------------------------

/// multi-core worker pool amortizing task synchronization across coarse chunks
/// inv: joins all active worker threads upon destruction; exception safe
class ThreadPool {
public:
    /// initialize worker threads with hardware concurrency or specified count
    /// in: threadCount number of threads (0 falls back to 1)
    /// own: pool manages thread lifecycle and joins workers on destruction
    /// thread: launches background worker threads
    /// err: joins launched workers and rethrows if thread spawn fails
    explicit ThreadPool(std::size_t threadCount = std::thread::hardware_concurrency()) : stopping_(false) {
        if (threadCount == 0) threadCount = 1;
        try {
            workers_.reserve(threadCount);
            for (std::size_t i = 0; i < threadCount; ++i) {
                workers_.emplace_back([this]() {
                    while (true) {
                        std::function<void()> task;
                        {
                            std::unique_lock<std::mutex> lock(mutex_);
                            cv_.wait(lock, [this]() { return stopping_ || !tasks_.empty(); });
                            if (stopping_ && tasks_.empty()) return;
                            task = std::move(tasks_.front());
                            tasks_.pop();
                        }
                        try {
                            task();
                        } catch (...) {
                            // Suppress task exceptions to prevent
                            // std::terminate on worker thread
                        }
                    }
                });
            }
        } catch (...) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                stopping_ = true;
            }
            cv_.notify_all();
            for (auto& worker : workers_) {
                if (worker.joinable()) worker.join();
            }
            throw;
        }
    }

    /// signal shutdown and join all worker threads
    /// inv: drains remaining tasks or exits cleanly; joins all threads
    /// thread: thread-safe, acquires internal mutex
    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) worker.join();
        }
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /// detaches all background worker threads
    /// inv: allows process exit without waiting for worker thread joins
    void detachWorkers() {
        for (auto& worker : workers_) {
            if (worker.joinable()) worker.detach();
        }
    }

    /// submit nullary callable to the task queue
    /// in: callable f
    /// out: std::future holding return value of f
    /// thread: thread-safe under mutex lock
    /// err: throws std::runtime_error if pool is stopping
    template <typename F> auto submit(F&& f) -> std::future<typename std::invoke_result<F>::type> {
        using ReturnType = typename std::invoke_result<F>::type;
        auto task = std::make_shared<std::packaged_task<ReturnType()>>(std::forward<F>(f));
        std::future<ReturnType> res = task->get_future();
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stopping_) throw std::runtime_error("ThreadPool is stopping");
            tasks_.emplace([task]() { (*task)(); });
        }
        cv_.notify_one();
        return res;
    }

    /// submit callable and arguments to the task queue
    /// in: callable f and bound arguments args
    /// out: std::future holding return value of f(args...)
    /// thread: thread-safe under mutex lock
    /// err: throws std::runtime_error if pool is stopping
    template <typename F, typename... Args>
        requires(sizeof...(Args) > 0)
    auto submit(F&& f, Args&&... args)
        -> std::future<typename std::invoke_result<std::decay_t<F>, std::decay_t<Args>&...>::type> {
        using ReturnType = typename std::invoke_result<std::decay_t<F>, std::decay_t<Args>&...>::type;
        auto task = std::make_shared<std::packaged_task<ReturnType()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        std::future<ReturnType> res = task->get_future();
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stopping_) throw std::runtime_error("ThreadPool is stopping");
            tasks_.emplace([task]() { (*task)(); });
        }
        cv_.notify_one();
        return res;
    }

    /// execute chunked parallel iteration over range [0..total)
    /// inv: chunking amortizes mutex acquisition; runs single-threaded if
    ///      total <= chunkSize
    /// in: total item count, chunkSize (0 auto-computes even partition),
    ///     fn(start, end)
    /// thread: blocks caller until all chunks complete; chunks run concurrently
    /// err: captures chunk exceptions and rethrows the first caught exception
    template <typename F> void parallelFor(std::size_t total, std::size_t chunkSize, F&& fn) {
        if (total == 0) return;
        if (chunkSize == 0) {
            const std::size_t count = workers_.empty() ? 1 : workers_.size();
            chunkSize = std::max<std::size_t>(1, total / count + (total % count != 0 ? 1 : 0));
        }
        if (workers_.empty() || total <= chunkSize) {
            fn(0, total);
            return;
        }

        const std::size_t numChunks = total / chunkSize + (total % chunkSize != 0 ? 1 : 0);
        std::vector<std::future<void>> futures;
        futures.reserve(numChunks);

        std::exception_ptr firstException = nullptr;
        try {
            for (std::size_t chunkIdx = 0; chunkIdx < numChunks; ++chunkIdx) {
                std::size_t start = chunkIdx * chunkSize;
                std::size_t end = std::min(start + chunkSize, total);
                futures.push_back(submit([&fn, start, end]() { fn(start, end); }));
            }
        } catch (...) {
            firstException = std::current_exception();
        }

        for (auto& fut : futures) {
            try {
                fut.get();
            } catch (...) {
                if (!firstException) {
                    firstException = std::current_exception();
                }
            }
        }

        if (firstException) {
            std::rethrow_exception(firstException);
        }
    }

    /// count of background worker threads in the pool
    [[nodiscard]] std::size_t workerCount() const noexcept { return workers_.size(); }

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> stopping_{false};
};

} // namespace razobill::core
