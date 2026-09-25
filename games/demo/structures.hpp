#pragma once
#include "world/world_generator.hpp"

namespace demo {
// Recipe bodies 0-3 of chunk (0, 0): the stone piers under the platform.
constexpr std::uint16_t platform_piers = 4;

// The demo's only generated building: four anchored piers carrying a small timber platform beside
// the spawn point. Pieces closer than 2.4 units are joined.
inline void generate_structures(void*, std::uint64_t /*seed*/, seed::ChunkCoord coord,
                                seed::ChunkBodies& out) {
    if (!(coord == seed::ChunkCoord{})) return;
    for (float y : {3.0F, 7.0F})
        for (float x : {-3.0F, 3.0F})
            seed::add_recipe_body(out, coord, {x, y}, {0.4F, 0.4F}, true, 0.9F);
    for (float y : {3.0F, 7.0F})
        for (float x : {-2.0F, 0.0F, 2.0F})
            seed::add_recipe_body(out, coord, {x, y}, {0.95F, 0.24F}, false, 0.9F);
    for (float x : {-3.0F, 3.0F})
        for (float y : {4.0F, 6.0F})
            seed::add_recipe_body(out, coord, {x, y}, {0.24F, 0.95F}, false, 0.9F);
    for (float x : {-2.0F, -1.0F, 0.0F, 1.0F, 2.0F})
        seed::add_recipe_body(out, coord, {x, 5}, {0.42F, 1.65F}, false, 0.9F);
    seed::join_recipe_bodies(out, 2.4F);
}
} // namespace demo
