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
inline seed::WorldGenerator generator() {
    seed::WorldGenerator generator;
    generator.name = "engine-test-flat";
    generator.version = 1;
    generator.terrain = terrain;
    generator.edit_bits = 3;
    generator.apply_edit = apply_edit;
    return generator;
}
} // namespace test_world
