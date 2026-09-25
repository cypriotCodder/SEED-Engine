#pragma once
#include "core/jobs.hpp"
#include <cstdint>
#include <filesystem>
#include <memory>

namespace seed {
// Holds an exclusive OS lock. Writers use working_directory(); only commit publishes a save.
class Checkpoint final {
public:
    enum class Stage { files_written, manifest_written, recovery_written, published };
    using Observer = void (*)(Stage); // Tests may interrupt a commit at a durable boundary.
    explicit Checkpoint(std::filesystem::path root);
    ~Checkpoint();
    Checkpoint(const Checkpoint&) = delete;
    Checkpoint& operator=(const Checkpoint&) = delete;
    const std::filesystem::path& working_directory() const { return working_; }
    // Resolve reads through working state and the frozen checkpoint layers. Callers must finish
    // existing readers before begin_commit/commit rotates working/ (World::save supplies that barrier).
    std::filesystem::path read_path(const std::string& filename) const;
    void commit(Observer observer = nullptr);
    // Call after freezing world/player deltas into working/. Subsequent writes must use write_blob.
    // Only the snapshot capture runs here; validation, flushes, and publication run on the job pool.
    bool begin_commit(Jobs& jobs, Observer observer = nullptr);
    bool finish_ready();  // Poll once per frame; rethrows background errors on the caller.
    void finish_commit(); // Join before shutdown or a synchronous commit.
    bool saving() const { return pending_; }
    struct Metrics {
        double snapshot_ms{}, publication_ms{};
        std::size_t linked_files{}, copied_files{}, moved_files{};
    };
    Metrics metrics() const; // Read after finishing a commit.

    bool recovered() const { return recovered_; }

private:
    struct Lock;
    struct Snapshot;
    struct ReadView {
        std::filesystem::path frozen, previous;
    };
    mutable std::mutex read_mutex_;
    ReadView read_view_;
    void set_read_view(std::filesystem::path frozen, std::filesystem::path previous);

    std::unique_ptr<Snapshot> prepare();
    void publish(const Snapshot& snapshot, Observer observer);
    static void publish_job(void* context) noexcept;
    Jobs* jobs_{};
    JobGroup group_;
    std::unique_ptr<Snapshot> snapshot_;
    Observer observer_{};
    std::exception_ptr error_;
    Metrics metrics_;
    bool pending_{};
    std::filesystem::path root_, working_;
    std::unique_ptr<Lock> lock_;
    std::uint64_t current_{};
    bool recovered_{};
};
} // namespace seed
