#pragma once
#include "core/jobs.hpp"
#include "core/memory.hpp"
#include "world/world_generator.hpp"
#include <atomic>
#include <exception>
#include <filesystem>
#include <functional>

namespace seed {
// Lets another system own per-chunk state while a chunk is active. All callbacks run on the main
// thread. `release` must copy state back into the chunk and set `dirty` if it changed; `store`
// does the same but keeps the state resident.
struct ChunkHooks {
    void* context{};
    void (*activate)(void*, ChunkCoord, Chunk&){};
    void (*release)(void*, ChunkCoord, Chunk&){};
    void (*store)(void*, ChunkCoord, Chunk&){};
};

class World final {
public:
    using ReadPath = std::function<std::filesystem::path(const std::filesystem::path&)>;
    World(Jobs& jobs, const WorldGenerator& generator, std::uint64_t seed, std::filesystem::path directory,
          ReadPath read_path = {});
    const WorldGenerator& generator() const { return generator_; }
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    void observe(ChunkHooks hooks) { hooks_ = hooks; }
    void stream(ChunkCoord center);
    void settle(ChunkCoord center);
    void save();
    struct GenerationMetrics {
        std::uint64_t chunks{}, nanoseconds{}, maximum_nanoseconds{};
    };
    GenerationMetrics generation_metrics() const {
        return {generated_.load(), generation_ns_.load(), generation_max_ns_.load()};
    }
    // Applies game-defined edit bits to a resident tile and records them in the chunk's saved
    // changes. Returns false if the tile's chunk is not active.
    bool edit(WorldPosition position, std::uint8_t bits);
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
    std::atomic<std::uint64_t> generated_{}, generation_ns_{}, generation_max_ns_{};
    Jobs& jobs_;
    JobGroup group_;
    ChunkHooks hooks_;
    WorldGenerator generator_;
    std::uint64_t seed_;
    std::filesystem::path directory_;
    ReadPath read_path_;
    Pool<Chunk, 49> pool_;
    std::array<Slot, 49> slots_;
};
} // namespace seed
