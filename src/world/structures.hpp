#pragma once
#include "world/coordinates.hpp"
#include <array>
#include <bitset>
#include <cstdint>

namespace seed {
// Building bodies belong to the chunk that generated them (recipe bodies) or the chunk they were
// placed in (built bodies). Ownership never changes: a body is resident exactly while its owner
// chunk is, and its state is saved in its owner's file. Bodies may stand in a neighbouring chunk,
// but never farther than one chunk from their owner; physics holds them at that boundary.
constexpr std::size_t chunk_body_capacity = 256;
constexpr std::size_t chunk_joint_capacity = 1024;

struct BodyState {
    WorldPosition position{}, previous{};
    Vec2 half{0.5F, 0.5F};
    float angle{}, previous_angle{}, height{}, previous_height{}, inverse_mass{1}, health{100};
    bool exists{true};
};

// Joints only link bodies of the same recipe, so both endpoints always share an owner chunk.
struct JointRecipe {
    std::uint16_t a{}, b{};
    float length{};
};

struct ChunkBodies {
    // Indices [0, recipe_count) are generated recipe bodies whose index is their stable ID.
    // Indices [recipe_count, count) are player-built bodies; they carry no joints and are
    // renumbered densely on save, so their IDs are not stable references.
    std::array<BodyState, chunk_body_capacity> bodies{};
    std::array<JointRecipe, chunk_joint_capacity> joints{};
    std::bitset<chunk_joint_capacity> broken;
    std::uint16_t recipe_count{}, count{}, joint_count{};
};

inline bool within_owner_reach(ChunkCoord owner, const BodyState& body) {
    return nearby(body.position.chunk, owner, 1) && nearby(body.previous.chunk, owner, 1);
}

namespace detail {
inline void add_recipe_body(ChunkBodies& out, ChunkCoord owner, Vec2 local, Vec2 half, bool fixed,
                            float height) {
    if (out.count == chunk_body_capacity) throw std::runtime_error("Chunk body recipe budget exhausted");
    BodyState body;
    body.position = {owner, local};
    body.position.move({});
    body.previous = body.position;
    body.half = half;
    body.inverse_mass = fixed ? 0.0F : 1.0F;
    body.height = body.previous_height = height;
    if (!within_owner_reach(owner, body)) throw std::logic_error("Recipe body placed outside owner reach");
    out.bodies[out.count++] = body;
}
} // namespace detail

// Deterministic building recipe for one chunk. The same seed and coordinate always produce the
// same bodies in the same order, which is what lets saves store only differences.
inline void generate_structures(ChunkBodies& out, std::uint64_t /*seed*/, ChunkCoord coord) {
    out.count = out.recipe_count = out.joint_count = 0;
    out.broken.reset();
    if (coord == ChunkCoord{}) {
        // Four anchored piers carry a small timber platform beside the spawn point.
        for (float y : {3.0F, 7.0F})
            for (float x : {-3.0F, 3.0F})
                detail::add_recipe_body(out, coord, {x, y}, {0.4F, 0.4F}, true, 0.9F);
        for (float y : {3.0F, 7.0F})
            for (float x : {-2.0F, 0.0F, 2.0F})
                detail::add_recipe_body(out, coord, {x, y}, {0.95F, 0.24F}, false, 0.9F);
        for (float x : {-3.0F, 3.0F})
            for (float y : {4.0F, 6.0F})
                detail::add_recipe_body(out, coord, {x, y}, {0.24F, 0.95F}, false, 0.9F);
        for (float x : {-2.0F, -1.0F, 0.0F, 1.0F, 2.0F})
            detail::add_recipe_body(out, coord, {x, 5}, {0.42F, 1.65F}, false, 0.9F);
    }
    for (std::uint16_t i = 0; i < out.count; ++i)
        for (std::uint16_t j = i + 1; j < out.count; ++j) {
            const float distance = length(relative(out.bodies[i].position, out.bodies[j].position));
            if (distance >= 2.4F) continue;
            if (out.joint_count == chunk_joint_capacity)
                throw std::runtime_error("Chunk joint recipe budget exhausted");
            out.joints[out.joint_count++] = {i, j, distance};
        }
    out.recipe_count = out.count;
}
} // namespace seed
