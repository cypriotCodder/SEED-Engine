#pragma once
#include "core/material.hpp"
#include "world/noise.hpp"
#include "world/world_generator.hpp"
#include <array>
#include <cstring>

// The demo's world: a finite disc of land with biome rings, surrounded by ocean.
namespace demo {
using namespace seed;

// Version 2: finite disc world with biome rings. Saves from other versions are rejected.
constexpr std::uint32_t generator_version = 2;

// The world is a disc of land this many chunks in radius, surrounded by endless ocean.
constexpr std::int64_t world_radius_chunks = 300;
constexpr double world_radius = static_cast<double>(world_radius_chunks) * chunk_side;

enum class Biome : std::uint8_t { ocean, beach, meadow, forest, swamp, plains, mountain, snow, count };

// The demo keeps each tile's moisture, bit for bit, in the tile's four game bytes.
inline float tile_moisture(const Tile& tile) {
    float moisture;
    std::memcpy(&moisture, tile.game.data(), sizeof moisture);
    return moisture;
}
inline void set_tile_moisture(Tile& tile, float moisture) {
    std::memcpy(tile.game.data(), &moisture, sizeof moisture);
}

// Demo edit bits. Trees are the demo's only solid tiles.
constexpr std::uint8_t edit_remove_tree = 1, edit_excavate = 2;
constexpr std::uint8_t edit_bits = edit_remove_tree | edit_excavate;
// Applies saved or live edit bits to a freshly generated or resident tile. Excavating turns the
// tile into shallow water and also clears any tree.
inline void apply_tile_edit(Tile& tile, std::uint8_t bits) {
    if (bits & (edit_remove_tree | edit_excavate)) tile.flags &= static_cast<std::uint8_t>(~tile_solid);
    if (bits & edit_excavate) {
        tile.material = static_cast<std::uint8_t>(Material::water);
        tile.elevation = -0.1F;
    }
}

namespace terrain {
// Independent noise streams. Changing any of these changes every generated world.
constexpr std::uint64_t warp_x = 0x6a09e667f3bcc908ULL, warp_y = 0xbb67ae8584caa73bULL;
constexpr std::uint64_t basin = 0x3c6ef372fe94f82bULL, moisture = 0xb5297a4dULL;
constexpr std::uint64_t moisture_large = 0xa54ff53a5f1d36f1ULL, ring = 0x510e527fade682d1ULL;
constexpr std::uint64_t temperature = 0x9b05688c2b3e6c1fULL, ring_detail = 0x1f83d9abfb41bd6bULL;
constexpr std::uint64_t wetland = 0x5be0cd19137e2179ULL, ridge = 0xcbbb9d5dc1059ed8ULL;

// Ring bands as fractions of the world radius, measured after the boundaries are warped. Area
// grows with the square of the radius, so inner rings are wide: meadows cover roughly 8% of the
// disc, the forest/swamp ring 22%, and the plains/mountain ring 25% before snow takes over.
constexpr float meadow_ring = 0.28F, forest_ring = 0.55F, plains_ring = 0.75F;

inline float smoothstep(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0F, 1.0F);
    return t * t * (3 - 2 * t);
}
} // namespace terrain

// Everything generation decides about one ground point.
struct TerrainSample {
    float elevation{}, moisture{}, temperature{};
    Biome biome{Biome::ocean};
    Material material{Material::deep_water};
};

// Pure function of seed and position. `local` may lie on the far chunk edge (32); positions are
// normalized first, so both sides of a chunk border give identical results.
inline TerrainSample sample_terrain(std::uint64_t seed, ChunkCoord chunk, Vec2 local) {
    WorldPosition position{chunk, local};
    position.move({});
    TerrainSample out;
    // Beyond the disc there is only ocean; skip noise and keep huge coordinates out of float math.
    if (!nearby(position.chunk, {}, static_cast<std::uint64_t>(world_radius_chunks) + 2)) {
        out.elevation = -1;
        out.temperature = -1;
        return out;
    }
    const double wx = static_cast<double>(position.chunk.x) * chunk_side + position.local.x;
    const double wy = static_cast<double>(position.chunk.y) * chunk_side + position.local.y;
    const float from_spawn = static_cast<float>(std::sqrt(wx * wx + wy * wy));
    // Low-frequency distortion of the distance from spawn. It bends both the coastline, giving
    // peninsulas and bays, and the biome ring boundaries.
    const float bend = perlin(seed ^ terrain::ring, position, 1024) * 0.12F +
                       perlin(seed ^ terrain::ring_detail, position, 256) * 0.04F;
    const float radius = static_cast<float>(from_spawn / world_radius) + bend;

    // Domain warp bends coastlines so they do not look like smooth noise blobs.
    auto warped = position;
    warped.move(
        Vec2{perlin(seed ^ terrain::warp_x, position, 256), perlin(seed ^ terrain::warp_y, position, 256)} *
        24);

    // Land mass: a raised centre so spawn is always on land, a coast falloff at the disc edge,
    // broad basins that become lakes and inlets, and fractal detail.
    const float spawn_rise = 0.7F * std::exp(-(from_spawn / 40) * (from_spawn / 40));
    const float coast = terrain::smoothstep(0.88F, 1.0F, radius) * 1.6F;
    const float basins = perlin(seed ^ terrain::basin, warped, 512) * 0.75F;
    // Mountain ranges follow the crest lines of a ridged noise field in the outer rings.
    const float crest = 1 - std::abs(perlin(seed ^ terrain::ridge, warped, 512)) * 2.5F;
    const float ranges = std::max(0.0F, crest - 0.62F) * 1.6F * terrain::smoothstep(0.45F, 0.6F, radius);
    out.elevation = 0.34F + spawn_rise - coast + basins + ranges + fractal(seed, warped) * 0.6F;

    out.moisture = fractal(seed ^ terrain::moisture, warped) +
                   perlin(seed ^ terrain::moisture_large, position, 512) * 0.5F;
    const float ring = radius;
    // Broad wetland regions decide where swamps form inside the forest ring.
    const float wetland = perlin(seed ^ terrain::wetland, position, 512) + out.moisture * 0.25F;
    // Colder towards the rim and at altitude.
    out.temperature = 1 - ring * 1.4F + perlin(seed ^ terrain::temperature, position, 512) * 0.15F -
                      std::max(0.0F, out.elevation - 0.85F) * 0.6F;
    // Only the highest peaks are cold enough for snow before the outer ring.

    if (out.elevation < 0) {
        out.biome = Biome::ocean;
        out.material = out.elevation < -0.25F ? Material::deep_water : Material::water;
    } else if (out.elevation < 0.06F) {
        out.biome = Biome::beach;
        out.material = Material::sand;
    } else if (out.temperature < -0.15F) {
        out.biome = Biome::snow;
        out.material = Material::snow;
    } else if (ring < terrain::meadow_ring) {
        out.biome = Biome::meadow;
        out.material = Material::grass;
    } else if (ring < terrain::forest_ring) {
        const bool wet_lowland = wetland > 0.12F && out.elevation < 0.45F;
        out.biome = wet_lowland ? Biome::swamp : Biome::forest;
        out.material = wet_lowland ? Material::mud : Material::forest_floor;
        // Swamps are pocked with standing water.
        if (wet_lowland && out.moisture > 0.3F && wetland > 0.2F) {
            out.elevation = -0.01F;
            out.material = Material::water;
        }
    } else if (out.elevation > (ring < terrain::plains_ring ? 0.55F : 0.38F)) {
        out.biome = Biome::mountain;
        out.material = Material::stone;
    } else {
        out.biome = Biome::plains;
        out.material = Material::dry_grass;
    }
    return out;
}

// Chance of a tree on one tile, as 1 in N. Zero means none. Replaced by vegetation in phase C2.
inline std::uint64_t tree_rarity(Biome biome) {
    switch (biome) {
    case Biome::meadow:
        return 19;
    case Biome::forest:
        return 5;
    case Biome::swamp:
        return 12;
    case Biome::plains:
        return 60;
    default:
        return 0;
    }
}

inline void generate_terrain(void*, std::uint64_t seed, ChunkCoord coord, Chunk& chunk) {
    for (int y = 0; y < chunk_side; ++y)
        for (int x = 0; x < chunk_side; ++x) {
            const Vec2 local{static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F};
            const auto sample = sample_terrain(seed, coord, local);
            auto& tile = chunk.tiles[static_cast<std::size_t>(y * chunk_side + x)];
            tile.elevation = sample.elevation;
            set_tile_moisture(tile, sample.moisture);
            tile.material = static_cast<std::uint8_t>(sample.material);
            const auto rarity = tree_rarity(sample.biome);
            const auto h = world_hash(seed, std::uint64_t(coord.x) * chunk_side + static_cast<unsigned>(x),
                                      std::uint64_t(coord.y) * chunk_side + static_cast<unsigned>(y));
            if (rarity && sample.elevation > 0.1F && h % rarity == 0) tile.flags |= tile_solid;
            // Keep the spawn clearing and the demo platform free of trees.
            if (nearby(coord, {}, 1)) {
                const float wx = static_cast<float>(coord.x) * chunk_side + local.x;
                const float wy = static_cast<float>(coord.y) * chunk_side + local.y;
                if (std::abs(wx) < 8 && std::abs(wy) < 8)
                    tile.flags &= static_cast<std::uint8_t>(~tile_solid);
            }
        }
}

inline void apply_edit(void*, Tile& tile, std::uint8_t bits) {
    apply_tile_edit(tile, bits);
}

inline WorldGenerator world_generator() {
    WorldGenerator generator;
    generator.name = "demo-disc";
    generator.version = generator_version;
    generator.terrain = generate_terrain;
    generator.edit_bits = edit_bits;
    generator.apply_edit = apply_edit;
    return generator;
}
} // namespace demo
