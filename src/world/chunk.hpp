#pragma once
#include "core/material.hpp"
#include "world/coordinates.hpp"
#include "world/structures.hpp"
#include <array>
#include <bitset>
#include <cstdint>
#include <vector>

namespace seed {
// Engine-defined tile flag bits. Every other meaning belongs to the game.
constexpr std::uint8_t tile_solid = 1; // Blocks movement and building.

// One ground cell. The engine reads elevation, material and flags; `game` holds four bytes the game
// defines for itself (the demo keeps its moisture value there).
struct Tile {
    float elevation{};
    std::uint8_t material{}, flags{};
    // An object standing on the tile, such as a tree or rock, drawn over the ground: 0 for none,
    // otherwise its material ID + 1.
    std::uint8_t object{};
    std::array<std::uint8_t, 4> game{};
};
static_assert(sizeof(Tile) == 12, "Tile grew; every resident chunk holds 1,024 of them");
constexpr std::uint8_t no_object = 0;
constexpr std::uint8_t tile_object(MaterialId material) {
    return static_cast<std::uint8_t>(material + 1);
}

// Saved scene entities owned by a chunk, kept in their chunk-file encoding while the chunk is
// loaded but not active. The engine turns them into scene entities on activation and back into
// records on release; see chunk_file.hpp for the record layout.
constexpr std::size_t chunk_entity_capacity = 1024;
constexpr std::size_t entity_payload_capacity = 4096; // Game bytes per entity.
struct ChunkEntities {
    std::vector<std::uint8_t> records;
    std::uint16_t count{};
    bool operator==(const ChunkEntities&) const = default;
};

struct Chunk {
    std::array<Tile, chunk_side * chunk_side> tiles{};
    // Saved edit bits per tile. The game defines what each bit means and how it changes a tile.
    std::array<std::uint8_t, chunk_side * chunk_side> changes{};
    // Tiles replaced outright (World::set_tile): saved whole, and restored over the generated and
    // edited tile on load.
    std::bitset<chunk_side * chunk_side> replaced;
    ChunkBodies bodies;
    ChunkEntities entities;
    bool dirty{};
};
} // namespace seed
