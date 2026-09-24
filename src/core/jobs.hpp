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
// Counts the outstanding jobs of one subsystem so it can wait for its own work only.
// A group must outlive every job submitted with it.
class JobGroup final {
public:
    JobGroup() = default;
    JobGroup(const JobGroup&) = delete;
    JobGroup& operator=(const JobGroup&) = delete;

private:
    friend class Jobs;
    std::size_t outstanding_{};
};

class Jobs final {
public:
    struct Job {
        void (*run)(void*) noexcept {};
        void* context{};
        JobGroup* group{};
    };
    explicit Jobs(unsigned count = 0) {
        if (!count) count = std::clamp(std::thread::hardware_concurrency(), 1U, 4U);
        try {
            for (unsigned i = 0; i < count; ++i)
                threads_.emplace_back([this] { worker(); });
        } catch (...) {
            stop();
            throw;
        }
    }
    ~Jobs() {
        wait();
        stop();
    }
    Jobs(const Jobs&) = delete;
    Jobs& operator=(const Jobs&) = delete;
    void submit(Job job) {
        if (!job.run) throw std::invalid_argument("Empty job");
        std::unique_lock lock(mutex_);
        space_.wait(lock, [this] { return queued_ < queue_.size(); });
        queue_[(head_ + queued_) % queue_.size()] = job;
        ++queued_;
        ++outstanding_;
        if (job.group) ++job.group->outstanding_;
        ready_.notify_one();
    }
    // Waits for every job, regardless of group.
    void wait() {
        std::unique_lock lock(mutex_);
        idle_.wait(lock, [this] { return outstanding_ == 0; });
    }
    // Waits only for jobs submitted with this group.
    void wait(JobGroup& group) {
        std::unique_lock lock(mutex_);
        idle_.wait(lock, [&group] { return group.outstanding_ == 0; });
    }
    bool busy(const JobGroup& group) {
        std::lock_guard lock(mutex_);
        return group.outstanding_ != 0;
    }

private:
    void stop() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        ready_.notify_all();
        for (auto& thread : threads_)
            if (thread.joinable()) thread.join();
    }
    void worker() {
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [this] { return stop_ || queued_; });
                if (stop_ && !queued_) return;
                job = queue_[head_];
                head_ = (head_ + 1) % queue_.size();
                --queued_;
                space_.notify_one();
            }
            job.run(job.context);
            {
                std::lock_guard lock(mutex_);
                --outstanding_;
                const bool group_idle = job.group && --job.group->outstanding_ == 0;
                if (!outstanding_ || group_idle) idle_.notify_all();
            }
        }
    }
    std::array<Job, 128> queue_{};
    std::mutex mutex_;
    std::condition_variable ready_, space_, idle_;
    std::vector<std::thread> threads_;
    std::size_t head_{}, queued_{}, outstanding_{};
    bool stop_{};
};
} // namespace seed
