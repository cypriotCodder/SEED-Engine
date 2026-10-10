#pragma once
#include "world/chunk.hpp"
#include <cstdint>
#include <string_view>

namespace seed {
// How a game builds its world. All callbacks must be pure functions of their arguments (the same
// seed and coordinate always give the same result), because saves store only differences from
// what they generate. `terrain` and `apply_edit` run on worker threads concurrently, so `context`
// must be safe to read from several threads.
struct WorldGenerator {
    void* context{};
    const char* name{};      // Stable identifier of this generator.
    std::uint32_t version{}; // Bump whenever the same seed would generate a different world.
    // Fill every tile of `chunk` for this coordinate. Tiles arrive zeroed.
    void (*terrain)(void*, std::uint64_t seed, ChunkCoord coord, Chunk& chunk){};
    // Optional building recipe for a chunk, built with add_recipe_body and join_recipe_bodies.
    // Runs on worker threads. Every body must stay within one chunk of `coord`.
    void (*structures)(void*, std::uint64_t seed, ChunkCoord coord, ChunkBodies& out){};
    // Game-defined tile edits. `edit_bits` lists every bit the game uses; `apply_edit` applies bits
    // to a freshly generated or resident tile. The engine records and saves the bits.
    std::uint8_t edit_bits{};
    void (*apply_edit)(void*, Tile& tile, std::uint8_t bits){};
    // How many materials are registered, so tiles read from saves can be checked; 0 skips the check.
    std::size_t materials{};
};

// Generates the building recipe of one chunk. Everything it adds is part of the recipe baseline
// that saves are compared against.
inline void generate_structures(const WorldGenerator& generator, std::uint64_t seed, ChunkCoord coord,
                                ChunkBodies& out) {
    out.count = out.recipe_count = out.joint_count = 0;
    out.broken.reset();
    if (generator.structures) generator.structures(generator.context, seed, coord, out);
    out.recipe_count = out.count;
}

// Generates one chunk from scratch: tiles, building recipe and cleared edit state.
inline void fill_chunk(const WorldGenerator& generator, std::uint64_t seed, ChunkCoord coord, Chunk& chunk) {
    // Reset in place: a Chunk is tens of kilobytes and this runs on worker-thread stacks.
    chunk.tiles.fill({});
    chunk.changes.fill(0);
    chunk.replaced.reset();
    chunk.entities.records.clear();
    chunk.entities.count = 0;
    chunk.dirty = false;
    generate_structures(generator, seed, coord, chunk.bodies);
    generator.terrain(generator.context, seed, coord, chunk);
}

// 64-bit FNV-1a hash of a stable name. Saves record these IDs, so a name must never change once
// saves exist; bump the generator version instead.
constexpr std::uint64_t stable_id(std::string_view name) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const char c : name) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 0x100000001b3ULL;
    }
    return hash;
}
inline std::uint64_t generator_id(const WorldGenerator& generator) {
    return stable_id(generator.name);
}

inline void validate(const WorldGenerator& generator) {
    if (!generator.name || !generator.version || !generator.terrain ||
        (generator.edit_bits && !generator.apply_edit))
        throw std::invalid_argument("A world generator needs a name, a version, terrain() and, if it has "
                                    "edit bits, apply_edit()");
}
} // namespace seed
