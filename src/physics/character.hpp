#pragma once
#include "physics/physics.hpp"
#include "world/world.hpp"
#include <cmath>

namespace seed {
// Which tiles a character may not enter. `tile` is null when its chunk is not resident. The
// default rule blocks unloaded and solid tiles; games add their own terrain rules (the demo also
// blocks water).
struct TileRule {
    void* context{};
    bool (*blocked)(void*, const Tile* tile){};
    bool bodies{true}; // Whether building bodies block too.
};

inline bool tile_blocks(const TileRule& rule, const Tile* tile) {
    return rule.blocked ? rule.blocked(rule.context, tile) : (!tile || (tile->flags & tile_solid));
}

// Whether an axis-aligned box centred at `center` touches a blocking tile or any building body.
inline bool box_blocked(const World& world, const Physics& physics, WorldPosition center, Vec2 half,
                        const TileRule& rule = {}) {
    if (rule.bodies && physics.blocks(center, half)) return true;
    auto corner = center;
    corner.move(Vec2{-half.x, -half.y});
    // Every tile the box overlaps, from its lower-left corner to just inside its upper-right edge.
    constexpr float inside = 1e-4F;
    const float x0 = std::floor(corner.local.x), y0 = std::floor(corner.local.y);
    const int columns = static_cast<int>(std::floor(corner.local.x + 2 * half.x - inside) - x0) + 1;
    const int rows = static_cast<int>(std::floor(corner.local.y + 2 * half.y - inside) - y0) + 1;
    for (int j = 0; j < rows; ++j)
        for (int i = 0; i < columns; ++i) {
            WorldPosition tile{corner.chunk,
                               {x0 + static_cast<float>(i) + 0.5F, y0 + static_cast<float>(j) + 0.5F}};
            tile.move({});
            if (tile_blocks(rule, world.tile(tile))) return true;
        }
    return false;
}

// Moves a box-shaped character by `delta`, sliding along obstacles: the full move if it is free,
// otherwise each axis separately. Suited to the small per-step moves of a fixed-step game; a delta
// larger than the box can pass through thin obstacles. Physics must be idle (call from Game::step).
inline WorldPosition move_character(const World& world, const Physics& physics, WorldPosition from,
                                    Vec2 delta, Vec2 half, const TileRule& rule = {}) {
    auto full = from;
    full.move(delta);
    if (!box_blocked(world, physics, full, half, rule)) return full;
    auto result = from;
    for (const Vec2 axis : {Vec2{delta.x, 0}, Vec2{0, delta.y}}) {
        if (axis.x == 0 && axis.y == 0) continue;
        auto candidate = result;
        candidate.move(axis);
        if (!box_blocked(world, physics, candidate, half, rule)) result = candidate;
    }
    return result;
}
} // namespace seed
