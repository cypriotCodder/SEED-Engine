#pragma once
#include "world/world_generator.hpp"

// A minimal generator for engine tests: flat dry ground everywhere. Edit bit 1 marks a tile, bit 2
// floods it. It exists so engine tests never depend on a particular game.
namespace test_world {
inline void terrain(void*, std::uint64_t, seed::ChunkCoord, seed::Chunk& chunk) {
    for (auto& tile : chunk.tiles) {
        tile.elevation = 0.5F;
        tile.material = 1;
    }
}
inline void apply_edit(void*, seed::Tile& tile, std::uint8_t bits) {
    if (bits & 1) tile.game[0] = 1;
    if (bits & 2) tile.elevation = -0.1F;
}
// Test fixture: 4 anchored piers (IDs 0-3) carrying 15 joined timbers at the origin chunk.
inline void structures(void*, std::uint64_t, seed::ChunkCoord coord, seed::ChunkBodies& out) {
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
inline seed::WorldGenerator generator() {
    seed::WorldGenerator generator;
    generator.name = "engine-test-flat";
    generator.version = 1;
    generator.terrain = terrain;
    generator.structures = structures;
    generator.edit_bits = 3;
    generator.apply_edit = apply_edit;
    return generator;
}
} // namespace test_world
