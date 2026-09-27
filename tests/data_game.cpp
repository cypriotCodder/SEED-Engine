// A game whose materials and world come entirely from project data (data_game_assets, written by
// the editor's starter island): no material or terrain code. The code only adds a walker and
// draws, until play mode and scripts make even that unnecessary.
#include "app/app.hpp"
#include "physics/character.hpp"
#include <cstdio>
#include <stdexcept>

namespace {
seed::Entity setup(void*, seed::Engine& engine, seed::WorldPosition spawn) {
    return engine.scene.create({spawn, spawn, 0}, {engine.materials.find("sand"), {0.6F, 0.6F}});
}

// The generated world matches the project's terrain: land at spawn, open sea past the rim.
void loaded(void*, seed::Engine& engine) {
    const auto* spawn = engine.world.tile({});
    if (!spawn || spawn->elevation <= 0) throw std::runtime_error("Spawn is not on generated land");
}

void step(void*, seed::Engine& engine, float dt) {
    auto& transform = *engine.scene.transforms.find(engine.focus);
    transform.previous = transform.position;
    transform.position =
        seed::move_character(engine.world, engine.physics, transform.position, {0, 2 * dt}, {0.25F, 0.25F});
}

void render(void*, seed::Engine& engine, const seed::View& view) {
    engine.world.each([&](seed::ChunkCoord coord, const seed::Chunk& chunk) {
        const auto offset = seed::relative({coord, {}}, view.camera);
        for (int y = 0; y < seed::chunk_side; ++y)
            for (int x = 0; x < seed::chunk_side; ++x) {
                const auto& tile = chunk.tiles[static_cast<std::size_t>(y * seed::chunk_side + x)];
                const float px = offset.x + static_cast<float>(x) + 0.5F,
                            py = offset.y + static_cast<float>(y) + 0.5F;
                engine.renderer.sprite(tile.material, px, py);
                if (tile.object != seed::no_object)
                    engine.renderer.sprite(static_cast<seed::MaterialId>(tile.object - 1), px, py);
            }
    });
    engine.draw_entities(view);
}

void shutdown(void*, seed::Engine& engine) {
    unsigned chunks = 0, land = 0;
    engine.world.each([&](seed::ChunkCoord, const seed::Chunk& chunk) {
        ++chunks;
        for (const auto& tile : chunk.tiles)
            land += tile.elevation > 0;
    });
    if (!chunks || !land) throw std::runtime_error("No generated land was resident");
    std::printf("Data game: %u chunks from terrain.json, %u land tiles, %u frames.\n", chunks, land,
                engine.frames);
}
} // namespace

int main(int argc, char** argv) {
    seed::Game game;
    game.id = "engine-data-test-game";
    game.name = "seed_data_game";
    game.default_seed = 1;
    game.default_save = "saves/data";
    game.project_assets = "data_game_assets";
    game.setup = setup;
    game.loaded = loaded;
    game.step = step;
    game.render = render;
    game.shutdown = shutdown;
    return seed::run(game, argc, argv);
}
