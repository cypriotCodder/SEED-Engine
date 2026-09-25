#pragma once
#include "world/coordinates.hpp"
#include "world/structures.hpp"
#include <array>
#include <cstdint>

namespace seed {
// Engine-defined tile flag bits. Every other meaning belongs to the game.
constexpr std::uint8_t tile_solid = 1; // Blocks movement and building.

// One ground cell. The engine reads elevation, material and flags; `game` holds four bytes the game
// defines for itself (the demo keeps its moisture value there).
struct Tile {
    float elevation{};
    std::uint8_t material{}, flags{};
    std::array<std::uint8_t, 4> game{};
};

struct Chunk {
    std::array<Tile, chunk_side * chunk_side> tiles{};
    // Saved edit bits per tile. The game defines what each bit means and how it changes a tile.
    std::array<std::uint8_t, chunk_side * chunk_side> changes{};
    ChunkBodies bodies;
    bool dirty{};
};
} // namespace seed
