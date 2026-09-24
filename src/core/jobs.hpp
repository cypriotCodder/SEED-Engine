#pragma once
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace seed {
class Jobs final {
public:
    struct Job { void (*run)(void*) noexcept{}; void* context{}; };
    explicit Jobs(unsigned count = 0) {
        if (!count) count=std::clamp(std::thread::hardware_concurrency(),1U,4U);
        try { for (unsigned i=0;i<count;++i) threads_.emplace_back([this] { worker(); }); }
        catch (...) { stop(); throw; }
    }
    ~Jobs() { wait(); stop(); }
    Jobs(const Jobs&) = delete;
    Jobs& operator=(const Jobs&) = delete;
    void submit(Job job) {
        if (!job.run) throw std::invalid_argument("Empty job");
        std::unique_lock lock(mutex_);
        space_.wait(lock,[this] { return queued_ < queue_.size(); });
        queue_[(head_+queued_)%queue_.size()]=job; ++queued_; ++outstanding_;
        ready_.notify_one();
    }
    void wait() {
        std::unique_lock lock(mutex_);
        idle_.wait(lock,[this] { return outstanding_ == 0; });
    }
private:
    void stop() {
        { std::lock_guard lock(mutex_); stop_=true; }
        ready_.notify_all();
        for (auto& thread:threads_) if (thread.joinable()) thread.join();
    }
    void worker() {
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock,[this] { return stop_ || queued_; });
                if (stop_ && !queued_) return;
                job=queue_[head_]; head_=(head_+1)%queue_.size(); --queued_; space_.notify_one();
            }
            job.run(job.context);
            { std::lock_guard lock(mutex_); --outstanding_; if (!outstanding_) idle_.notify_all(); }
        }
    }
    std::array<Job,128> queue_{};
    std::mutex mutex_;
    std::condition_variable ready_, space_, idle_;
    std::vector<std::thread> threads_;
    std::size_t head_{}, queued_{}, outstanding_{};
    bool stop_{};
};
} // namespace seed
