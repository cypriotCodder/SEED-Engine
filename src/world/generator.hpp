#pragma once
#include "world/noise.hpp"
#include "world/structures.hpp"
#include <array>

namespace seed {
constexpr std::uint32_t generator_version = 1;
struct Tile {
    float elevation{}, moisture{};
    std::uint8_t material{};
    bool tree{};
};
struct Chunk {
    std::array<Tile, chunk_side * chunk_side> tiles{};
    std::array<std::uint8_t, chunk_side * chunk_side> changes{};
    ChunkBodies bodies;
    bool dirty{};
};
inline void generate(Chunk& chunk, std::uint64_t seed, ChunkCoord coord) {
    // Reset in place: a Chunk is tens of kilobytes and this runs on worker-thread stacks.
    chunk.tiles.fill({});
    chunk.changes.fill(0);
    chunk.dirty = false;
    generate_structures(chunk.bodies, seed, coord);
    for (int y = 0; y < chunk_side; ++y)
        for (int x = 0; x < chunk_side; ++x) {
            const Vec2 local{static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F};
            const auto index = static_cast<std::size_t>(y * chunk_side + x);
            auto& tile = chunk.tiles[index];
            float island = -2;
            if (nearby(coord, {}, 4)) {
                const float wx = static_cast<float>(coord.x) * chunk_side + local.x;
                const float wy = static_cast<float>(coord.y) * chunk_side + local.y;
                island = 0.75F - (wx * wx + wy * wy) / 3600;
            }
            tile.elevation = island + fractal(seed, coord, local) * 0.9F;
            tile.moisture = fractal(seed ^ 0xb5297a4dULL, coord, local);
            tile.material = tile.elevation < 0 ? 0
                            : tile.elevation < 0.1F
                                ? 1
                                : (tile.elevation > 0.4F && tile.moisture < -0.1F ? 3 : 2);
            const auto h = world_hash(seed, std::uint64_t(coord.x) * chunk_side + static_cast<unsigned>(x),
                                      std::uint64_t(coord.y) * chunk_side + static_cast<unsigned>(y));
            tile.tree = tile.material == 2 && tile.elevation > 0.16F && h % 19 == 0;
            if (nearby(coord, {}, 1)) {
                const float wx = static_cast<float>(coord.x) * chunk_side + local.x;
                const float wy = static_cast<float>(coord.y) * chunk_side + local.y;
                if (std::abs(wx) < 8 && std::abs(wy) < 8) tile.tree = false;
            }
        }
}
} // namespace seed
