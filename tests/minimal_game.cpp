// The smallest complete game, used to prove the engine is usable without the demo: it links only
// seed_engine, reads its materials from a project assets folder made the way the editor writes it,
// and generates, edits, streams, saves and reloads a world, including a saved entity with a game
// component and the game's own state.
#include "app/app.hpp"
#include "physics/character.hpp"
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace {
// IDs follow the order of minimal_game_assets/materials.json; setup() checks they still match.
enum : seed::MaterialId { meadow, pond };
constexpr std::uint8_t flood = 1;

// A game component stored in a saved entity's payload.
struct Beacon {
    std::uint8_t level{};
};
constexpr seed::WorldPosition beacon_at{{}, {8.5F, 4.5F}};

// Flat meadow with a pond stripe two tiles wide at x = 20 in every chunk; pond tiles block movement.
void terrain(void*, std::uint64_t, seed::ChunkCoord, seed::Chunk& chunk) {
    for (int y = 0; y < seed::chunk_side; ++y)
        for (int x = 0; x < seed::chunk_side; ++x) {
            auto& tile = chunk.tiles[static_cast<std::size_t>(y * seed::chunk_side + x)];
            tile.material = (x == 20 || x == 21) ? pond : meadow;
            tile.flags = (x == 20 || x == 21) ? seed::tile_solid : 0;
        }
}

void apply_edit(void*, seed::Tile& tile, std::uint8_t bits) {
    if (bits & flood) {
        tile.material = pond;
        tile.flags |= seed::tile_solid;
    }
}

void save_entity(void*, seed::Engine& engine, seed::Entity entity, seed::Bytes& out) {
    out.u8(engine.scene.components<Beacon>().find(entity)->level);
}
void load_entity(void*, seed::Engine& engine, seed::Entity entity, seed::Reader& in) {
    engine.scene.components<Beacon>().add(entity, {in.u8()});
}

// The game's own state: how many times it has run on this save.
std::uint32_t runs = 0;
void save_state(void*, seed::Engine&, seed::Bytes& out) {
    out.u32(runs);
}
void load_state(void*, seed::Engine&, seed::Reader& in) {
    runs = in.u32();
}

// The single beacon in the scene, or nullptr.
const Beacon* beacon(seed::Engine& engine, seed::WorldPosition* position = nullptr) {
    auto& beacons = engine.scene.components<Beacon>();
    if (beacons.values().size() > 1) throw std::runtime_error("Saved beacon was duplicated");
    if (beacons.values().empty()) return nullptr;
    if (position) *position = engine.scene.transforms.find(beacons.owners()[0])->position;
    return &beacons.values()[0];
}

seed::Entity setup(void*, seed::Engine& engine, seed::WorldPosition spawn) {
    if (engine.materials.find("meadow") != meadow || engine.materials.find("pond") != pond)
        throw std::runtime_error("materials.json no longer matches the game's material IDs");
    engine.scene.add_component<Beacon>();
    ++runs;
    return engine.scene.create({spawn, spawn, 0}, {meadow, {0.5F, 0.5F}});
}

// Flood a tile and place a beacon, stream their chunk out (which saves it) and back in, and check
// both survived. A run on an existing save must find the beacon the first run left.
void loaded(void*, seed::Engine& engine) {
    const bool resumed = std::filesystem::exists(engine.checkpoint.read_path("player.delta"));
    if (resumed && !beacon(engine)) throw std::runtime_error("Saved beacon did not survive a restart");
    if (runs != (resumed ? 2u : 1u)) throw std::runtime_error("Game state did not survive a restart");
    if (!resumed) {
        if (beacon(engine)) throw std::runtime_error("A fresh save already has a beacon");
        const auto entity = engine.create_saved({beacon_at, beacon_at, 0}, {pond, {0.4F, 0.4F}});
        engine.scene.components<Beacon>().add(entity, {7});
    }
    const seed::WorldPosition target{{}, {5.5F, 5.5F}};
    if (!engine.world.edit(target, flood)) throw std::runtime_error("Could not edit a resident tile");
    engine.world.settle({12, 12});
    if (engine.world.tile(target)) throw std::runtime_error("Chunk stayed resident after moving away");
    if (beacon(engine)) throw std::runtime_error("Beacon stayed in the scene after its chunk unloaded");
    engine.world.settle({});
    const auto* tile = engine.world.tile(target);
    if (!tile || tile->material != pond || !(tile->flags & seed::tile_solid))
        throw std::runtime_error("Tile edit did not survive unload and reload");
    seed::WorldPosition position;
    const auto* restored = beacon(engine, &position);
    if (!restored || restored->level != 7 || position.chunk != beacon_at.chunk ||
        position.local.x != beacon_at.local.x || position.local.y != beacon_at.local.y)
        throw std::runtime_error("Beacon did not survive unload and reload");
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
    // A small HUD: a translucent panel with the walked distance.
    const auto label = "Walked " + std::to_string(static_cast<int>(engine.focus_position().local.x * 100)) +
                       " cm\nSeed Engine UI";
    engine.renderer.ui_rect(12, 12, seed::Renderer::text_width(label) + 16, 44, {0, 0, 0, 0.55F});
    engine.renderer.text(20, 18, label, 2, {1, 0.95F, 0.7F, 1});
}

void shutdown(void*, seed::Engine& engine) {
    unsigned chunks = 0;
    engine.world.each([&](seed::ChunkCoord, const seed::Chunk&) { ++chunks; });
    const auto moved = engine.focus_position().local.x;
    if (!chunks || moved <= 0) throw std::runtime_error("Minimal game did not stream or move");
    if (!beacon(engine)) throw std::runtime_error("Beacon was lost during play");
    std::printf("Minimal game: run %u, %u frames, %u resident chunks, walked to x=%.2f, beacon kept.\n",
                static_cast<unsigned>(runs), engine.frames, chunks, moved);
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
    game.project_assets = "minimal_game_assets";
    game.save_entity = save_entity;
    game.load_entity = load_entity;
    game.save_state = save_state;
    game.load_state = load_state;
    game.setup = setup;
    game.loaded = loaded;
    game.step = step;
    game.render = render;
    game.shutdown = shutdown;
    return seed::run(game, argc, argv);
}
