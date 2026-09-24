#pragma once
#include "core/jobs.hpp"
#include "core/memory.hpp"
#include "world/generator.hpp"
#include <atomic>
#include <exception>
#include <filesystem>

namespace seed {
class World final {
public:
    World(Jobs& jobs, std::uint64_t seed, std::filesystem::path directory);
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    void stream(ChunkCoord center);
    void settle(ChunkCoord center);
    void save();
    bool remove_tree(WorldPosition position);
    bool dig(WorldPosition position);
    const Tile* tile(WorldPosition position) const;
    template<class F>
    void each(F&& visitor) const {
        for (const auto& slot : slots_)
            if (slot.state.load(std::memory_order_acquire) == State::active) visitor(slot.coord, *slot.chunk);
    }

private:
    enum class State { empty, generating, ready, active, saving, saved, failed };
    struct Slot {
        World* world{};
        Chunk* chunk{};
        ChunkCoord coord{};
        std::atomic<State> state{State::empty};
        std::exception_ptr error;
    };
    static void generate_job(void* context) noexcept;
    static void save_job(void* context) noexcept;
    std::filesystem::path path(ChunkCoord coord) const;
    void load(Slot& slot);
    void write(Slot& slot);
    void check_errors() const;
    Jobs& jobs_;
    std::uint64_t seed_;
    std::filesystem::path directory_;
    Pool<Chunk, 49> pool_;
    std::array<Slot, 49> slots_;
};
} // namespace seed
