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

// Recipe helpers for WorldGenerator::structures. Bodies are numbered in the order they are added;
// that index is the body's stable ID, so a recipe must always add them in the same order.
inline std::uint16_t add_recipe_body(ChunkBodies& out, ChunkCoord owner, Vec2 local, Vec2 half, bool fixed,
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
    out.bodies[out.count] = body;
    return out.count++;
}

// Joins every pair of recipe bodies whose centres are closer than `max_distance`, at their current
// separation. Joint IDs follow pair order, so they are stable for a stable recipe.
inline void join_recipe_bodies(ChunkBodies& out, float max_distance) {
    for (std::uint16_t i = 0; i < out.count; ++i)
        for (std::uint16_t j = i + 1; j < out.count; ++j) {
            const float distance = length(relative(out.bodies[i].position, out.bodies[j].position));
            if (distance >= max_distance) continue;
            if (out.joint_count == chunk_joint_capacity)
                throw std::runtime_error("Chunk joint recipe budget exhausted");
            out.joints[out.joint_count++] = {i, j, distance};
        }
}
} // namespace seed
