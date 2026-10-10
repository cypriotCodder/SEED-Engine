#pragma once
#include "core/jobs.hpp"
#include "core/memory.hpp"
#include "world/world_generator.hpp"
#include <atomic>
#include <exception>
#include <filesystem>
#include <functional>
#include <string>

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
    // Opens or creates the save in `directory`. world.seed binds it to one game, one generator
    // version and one seed; opening it with anything else throws without changing it.
    // `prefix` starts every file name, so one save can hold several worlds (a game's scenes).
    World(Jobs& jobs, std::uint64_t game_id, const WorldGenerator& generator, std::uint64_t seed,
          std::filesystem::path directory, ReadPath read_path = {}, std::string prefix = {});
    // Writes and unloads every chunk, then continues as the world of another generator, seed and
    // file prefix in the same save. Nothing is loaded until the next stream or settle. Physics
    // must be idle.
    void reopen(const WorldGenerator& generator, std::uint64_t seed, std::string prefix);
    const std::string& prefix() const { return prefix_; }
    std::uint64_t seed() const { return seed_; }
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
    // Replaces a resident tile outright and saves it whole. Throws for a material or object the
    // generator's `materials` does not cover, or a non-finite elevation; returns false if the tile's
    // chunk is not active.
    bool set_tile(WorldPosition position, const Tile& tile);
    const Tile* tile(WorldPosition position) const;
    bool active(ChunkCoord coord) const;
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
    void open();
    std::atomic<std::uint64_t> generated_{}, generation_ns_{}, generation_max_ns_{};
    Jobs& jobs_;
    JobGroup group_;
    ChunkHooks hooks_;
    WorldGenerator generator_;
    std::uint64_t game_id_, seed_;
    std::filesystem::path directory_;
    ReadPath read_path_;
    std::string prefix_;
    Pool<Chunk, 49> pool_;
    std::array<Slot, 49> slots_;
};
} // namespace seed
