#pragma once
#include "core/jobs.hpp"
#include "core/scene.hpp"
#include "physics/collision.hpp"
#include "world/world.hpp"
#include <array>
#include <memory>
#include <optional>

namespace seed {
// Ground-plane building physics for the bodies of resident chunks.
//
// Bodies and joints live in fixed pools shared by all chunks. A chunk's bodies enter the pools
// when World activates the chunk (attach) and leave when it unloads (release), so the pools only
// ever hold the neighbourhood around the player.
//
// A chunk's bodies are simulated only when the chunk lies within `simulation_radius` of the
// anchor and all eight neighbouring chunks are resident, so a moving body can never miss a
// neighbour that has not loaded yet. Bodies of resident chunks one ring farther out still take part
// in collisions, as immovable obstacles.
//
// A step runs on a worker between begin_step and finish_step. In that window the pools belong to
// the worker: every other member function throws std::logic_error rather than race with it.
class Physics final {
public:
    static constexpr std::size_t body_capacity = 4096;
    static constexpr std::size_t joint_capacity = 16384;
    static constexpr std::size_t links_per_body = 16;
    static constexpr std::size_t resident_capacity = 64;
    static constexpr std::uint64_t simulation_radius = 1;

    Physics(Scene& scene, Jobs& jobs);
    ~Physics();
    Physics(const Physics&) = delete;
    Physics& operator=(const Physics&) = delete;

    // Callbacks for World::observe.
    ChunkHooks hooks();
    void attach(ChunkCoord owner, const ChunkBodies& bodies);
    // Copies resident state into `bodies`. Returns true if anything differs from what it held.
    bool store(ChunkCoord owner, ChunkBodies& bodies) const;
    // Stores, then removes the chunk's bodies and joints from the simulation.
    bool release(ChunkCoord owner, ChunkBodies& bodies);

    void begin_step(WorldPosition anchor);
    void finish_step();
    void step(WorldPosition anchor) {
        begin_step(anchor);
        finish_step();
    }
    bool stepping() const { return pending_; }
    std::uint64_t last_step_nanoseconds() const {
        require_idle();
        return last_step_ns_;
    }

    bool damage(WorldPosition target, float amount);
    bool build(WorldPosition target);
    bool blocks(WorldPosition position) const;
    void collapse_demo();
    // Position of an existing resident recipe body, or nothing if it is destroyed or not resident.
    std::optional<WorldPosition> find(ChunkCoord owner, std::uint16_t recipe_id) const;
    std::size_t count() const;
    unsigned unsupported() const;
    unsigned grounded_unsupported() const;

private:
    static constexpr std::uint16_t built_id = 0xffff;
    static constexpr std::uint16_t none = 0xffff;
    struct Body {
        BodyState state;
        ChunkCoord owner{};
        std::uint16_t id{};      // Recipe ID, the chunk index of a loaded built body, or built_id.
        std::uint8_t resident{}; // Index of the owner's Resident entry.
        bool live{}, supported{};
        Entity entity{};
        std::array<std::uint16_t, links_per_body> links{}; // Joint indices.
        std::uint8_t link_count{};
    };
    struct Joint {
        ChunkCoord owner{};
        std::uint16_t a{}, b{}, recipe{};
        float length{};
        bool live{}, broken{};
    };
    struct CellEntry {
        int x{}, y{}, next{-1};
        std::uint16_t body{};
    };
    struct Resident {
        ChunkCoord coord{};
        std::uint16_t recipe_count{};
        bool used{};
        bool simulate{}, collide{}; // Decided at the start of each step.
    };
    // Allocated once; far too large for the stack and never resized.
    struct Storage {
        std::array<Body, body_capacity> bodies{};
        std::array<Joint, joint_capacity> joints{};
        std::array<std::uint16_t, body_capacity> free_bodies{};
        std::array<std::uint16_t, joint_capacity> free_joints{};
        std::array<int, 4096> heads{};
        std::array<CellEntry, body_capacity * 16> entries{};
        std::array<std::array<int, 2>, body_capacity> first_cell{};
        std::array<std::uint16_t, body_capacity> queue{};
        std::array<Resident, resident_capacity> residents{};
    };

    static void job(void* context) noexcept;
    void require_idle() const;
    std::uint16_t allocate_body(const BodyState& state, ChunkCoord owner, std::uint16_t id,
                                std::uint8_t resident);
    void free_body(std::uint16_t index);
    void link(std::uint16_t body, std::uint16_t joint);
    Resident* resident(ChunkCoord coord);
    const Resident* resident(ChunkCoord coord) const;
    void plan_step();
    bool active(const Body& body) const;
    bool collider(const Body& body) const;
    bool joined(std::uint16_t a, std::uint16_t b) const;
    void simulate();
    void support();
    void contacts();
    void sync_scene();

    Scene& scene_;
    Jobs& jobs_;
    JobGroup group_;
    std::unique_ptr<Storage> storage_;
    std::size_t free_body_count_{}, free_joint_count_{};
    std::size_t body_end_{}, joint_end_{}; // One past the highest slot ever used.
    WorldPosition anchor_{};
    std::exception_ptr error_;
    std::uint64_t last_step_ns_{}; // Published by the job-group completion barrier.
    bool pending_{}, support_dirty_{true};
};
} // namespace seed
