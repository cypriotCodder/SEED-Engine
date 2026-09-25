// The smallest complete game, used to prove the engine is usable without the demo: it links only
// seed_engine, ships no assets, and generates, edits, streams, saves and reloads a world.
#include "app/app.hpp"
#include "physics/character.hpp"
#include <cstdio>
#include <stdexcept>

namespace {
enum : seed::MaterialId { meadow, pond };
constexpr std::uint8_t flood = 1;

void materials(void*, seed::Materials& registry) {
    registry.add({"meadow", {90, 140, 70}});
    registry.add({"pond", {30, 80, 120}, seed::Pattern::water});
}

// Flat meadow with a pond stripe along every chunk's right edge; pond tiles block movement.
void terrain(void*, std::uint64_t, seed::ChunkCoord, seed::Chunk& chunk) {
    for (int y = 0; y < seed::chunk_side; ++y)
        for (int x = 0; x < seed::chunk_side; ++x) {
            auto& tile = chunk.tiles[static_cast<std::size_t>(y * seed::chunk_side + x)];
            tile.material = x >= 30 ? pond : meadow;
            tile.flags = x >= 30 ? seed::tile_solid : 0;
        }
}

void apply_edit(void*, seed::Tile& tile, std::uint8_t bits) {
    if (bits & flood) {
        tile.material = pond;
        tile.flags |= seed::tile_solid;
    }
}

seed::Entity setup(void*, seed::Engine& engine, seed::WorldPosition spawn) {
    return engine.scene.create({spawn, spawn, 0}, {meadow, {0.5F, 0.5F}});
}

// Flood a tile, stream its chunk out (which saves it) and back in, and check the edit survived.
void loaded(void*, seed::Engine& engine) {
    const seed::WorldPosition target{{}, {5.5F, 5.5F}};
    if (!engine.world.edit(target, flood)) throw std::runtime_error("Could not edit a resident tile");
    engine.world.settle({12, 12});
    if (engine.world.tile(target)) throw std::runtime_error("Chunk stayed resident after moving away");
    engine.world.settle({});
    const auto* tile = engine.world.tile(target);
    if (!tile || tile->material != pond || !(tile->flags & seed::tile_solid))
        throw std::runtime_error("Tile edit did not survive unload and reload");
}

// Walk east; the engine's character mover stops at solid tiles.
void step(void*, seed::Engine& engine, float dt) {
    auto& transform = *engine.scene.transforms.find(engine.focus);
    transform.previous = transform.position;
    transform.position =
        seed::move_character(engine.world, engine.physics, transform.position, {2 * dt, 0}, {0.25F, 0.25F});
}

void render(void*, seed::Engine& engine, const seed::View& view) {
    engine.world.each([&](seed::ChunkCoord coord, const seed::Chunk& chunk) {
        const auto offset = seed::relative({coord, {}}, view.camera);
        for (int y = 0; y < seed::chunk_side; ++y)
            for (int x = 0; x < seed::chunk_side; ++x)
                engine.renderer.sprite(
                    chunk.tiles[static_cast<std::size_t>(y * seed::chunk_side + x)].material,
                    offset.x + x + 0.5F, offset.y + y + 0.5F);
    });
    engine.draw_entities(view);
    engine.renderer.light(0, 0, 20, 1, 1, 1, 1, 4);
}

void shutdown(void*, seed::Engine& engine) {
    unsigned chunks = 0;
    engine.world.each([&](seed::ChunkCoord, const seed::Chunk&) { ++chunks; });
    const auto moved = engine.focus_position().local.x;
    if (!chunks || moved <= 0) throw std::runtime_error("Minimal game did not stream or move");
    std::printf("Minimal game: %u frames, %u resident chunks, walked to x=%.2f.\n", engine.frames, chunks,
                moved);
}
} // namespace

int main(int argc, char** argv) {
    seed::Game game;
    game.id = "engine-minimal-test-game";
    game.name = "seed_minimal_game";
    game.title = "Seed Engine minimal game";
    game.default_seed = 1;
    game.default_save = "saves/minimal";
    game.world.name = "minimal-flat";
    game.world.version = 1;
    game.world.terrain = terrain;
    game.world.edit_bits = flood;
    game.world.apply_edit = apply_edit;
    game.materials = materials;
    game.setup = setup;
    game.loaded = loaded;
    game.step = step;
    game.render = render;
    game.shutdown = shutdown;
    return seed::run(game, argc, argv);
}
