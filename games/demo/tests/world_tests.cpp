#include "terrain.hpp"
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void repeatable_and_seed_dependent() {
    const auto generator = demo::world_generator();
    auto a = std::make_unique<seed::Chunk>(), b = std::make_unique<seed::Chunk>();
    auto c = std::make_unique<seed::Chunk>();
    for (const seed::ChunkCoord coord :
         {seed::ChunkCoord{0, 0}, seed::ChunkCoord{-40, 97}, seed::ChunkCoord{150, -210}}) {
        seed::fill_chunk(generator, 123, coord, *a);
        seed::fill_chunk(generator, 123, coord, *b);
        seed::fill_chunk(generator, 124, coord, *c);
        bool different = false;
        for (std::size_t i = 0; i < a->tiles.size(); ++i) {
            const auto &x = a->tiles[i], &y = b->tiles[i];
            check(x.elevation == y.elevation && x.material == y.material && x.flags == y.flags &&
                      x.game == y.game,
                  "Generation must be repeatable");
            different |= x.elevation != c->tiles[i].elevation;
        }
        check(different, "Seed must affect generation");
    }
}

void seams() {
    // The same point addressed from both sides of a chunk border must give the same terrain,
    // including the domain-warped layers that sample neighbouring chunks.
    for (std::int64_t c = -5; c <= 5; ++c)
        for (float t : {0.25F, 7.5F, 31.75F}) {
            const auto left = demo::sample_terrain(9, {c - 1, 3}, {32, t});
            const auto right = demo::sample_terrain(9, {c, 3}, {0, t});
            check(left.elevation == right.elevation && left.moisture == right.moisture &&
                      left.temperature == right.temperature && left.biome == right.biome,
                  "Terrain seam across x");
            const auto below = demo::sample_terrain(9, {3, c - 1}, {t, 32});
            const auto above = demo::sample_terrain(9, {3, c}, {t, 0});
            check(below.elevation == above.elevation && below.biome == above.biome, "Terrain seam across y");
        }
}

void world_shape() {
    for (std::uint64_t seed : {1ULL, 2ULL, 99ULL, 20260923ULL}) {
        // Spawn is always dry meadow, so the demo platform and player start on land.
        for (float x : {-3.0F, 0.0F, 3.0F})
            for (float y : {0.0F, 3.0F, 7.0F}) {
                seed::WorldPosition p{{}, {x, y}};
                p.move({});
                const auto s = demo::sample_terrain(seed, p.chunk, p.local);
                check(s.biome == demo::Biome::meadow && s.elevation > 0.1F, "Spawn area must be meadow");
            }
        // Beyond the disc is ocean, including absurdly distant coordinates.
        const auto rim = demo::world_radius_chunks + 5;
        check(demo::sample_terrain(seed, {rim, 0}, {1, 1}).biome == demo::Biome::ocean, "Ocean past the rim");
        check(demo::sample_terrain(seed, {INT64_MAX, INT64_MIN}, {1, 1}).biome == demo::Biome::ocean,
              "Ocean at extreme coordinates");
        // Every biome appears somewhere on the disc.
        std::array<bool, static_cast<std::size_t>(demo::Biome::count)> seen{};
        for (std::int64_t cy = -demo::world_radius_chunks; cy <= demo::world_radius_chunks; cy += 7)
            for (std::int64_t cx = -demo::world_radius_chunks; cx <= demo::world_radius_chunks; cx += 7)
                seen[static_cast<std::size_t>(demo::sample_terrain(seed, {cx, cy}, {16, 16}).biome)] = true;
        for (bool biome : seen)
            check(biome, "Every biome must appear on the disc");
        // Rings run warm to cold: meadows near spawn, snow near the rim.
        check(demo::sample_terrain(seed, {3, 3}, {16, 16}).temperature >
                  demo::sample_terrain(seed, {0, 260}, {16, 16}).temperature,
              "Temperature falls towards the rim");
    }
}

void generation_time() {
    const auto generator = demo::world_generator();
    auto chunk = std::make_unique<seed::Chunk>();
    const auto start = std::chrono::steady_clock::now();
    constexpr int chunks = 50;
    for (int i = 0; i < chunks; ++i)
        seed::fill_chunk(generator, 5, {i * 5 - 120, 40 - i * 3}, *chunk);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / chunks;
    std::cout << "Chunk generation: " << ms << " ms per chunk in this build.\n";
    // Loose bound that holds even unoptimized; optimized builds measure about 0.4 ms.
    check(ms < 25, "Chunk generation is far over budget");
}
} // namespace

int main() {
    try {
        repeatable_and_seed_dependent();
        seams();
        world_shape();
        generation_time();
        std::cout << "World repeatability, seam, biome and ring checks passed.\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
