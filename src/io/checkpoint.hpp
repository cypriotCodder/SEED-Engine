#pragma once
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
    void commit(Observer observer = nullptr);
    bool recovered() const { return recovered_; }

private:
    struct Lock;
    std::filesystem::path root_, working_;
    std::unique_ptr<Lock> lock_;
    std::uint64_t current_{};
    bool recovered_{};
};
} // namespace seed
