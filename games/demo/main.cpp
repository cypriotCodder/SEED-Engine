// The demo game: explore a procedural island, damage a timber platform, dig and build.
#include "app/app.hpp"
#include "physics/character.hpp"
#include "terrain.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string_view>

namespace {
// Digging turns dry ground into water; felling removes a tree. Both return false if nothing changed.
bool dig(seed::World& world, seed::WorldPosition position) {
    const auto* tile = world.tile(position);
    return tile && tile->elevation >= 0 && world.edit(position, demo::edit_remove_tree | demo::edit_excavate);
}
bool fell(seed::World& world, seed::WorldPosition position) {
    const auto* tile = world.tile(position);
    return tile && (tile->flags & seed::tile_solid) && world.edit(position, demo::edit_remove_tree);
}

struct Demo {
    struct {
        seed::ActionId left, right, up, down, map, damage, dig, build;
    } action{};
    bool damage_demo{}, overview{}, verify_stream{};
    float damage_cooldown{};
    seed::WorldPosition spawn;
};

void register_actions(void* context, seed::Actions& actions) {
    using seed::Binding;
    auto& action = static_cast<Demo*>(context)->action;
    action.left = actions.add("move_left", {Binding::key(SDL_SCANCODE_A)});
    action.right = actions.add("move_right", {Binding::key(SDL_SCANCODE_D)});
    action.up = actions.add("move_up", {Binding::key(SDL_SCANCODE_W)});
    action.down = actions.add("move_down", {Binding::key(SDL_SCANCODE_S)});
    action.map = actions.add("toggle_map", {Binding::key(SDL_SCANCODE_TAB)});
    action.damage = actions.add("damage", {Binding::mouse(SDL_BUTTON_LEFT)});
    action.dig = actions.add("dig", {Binding::mouse(SDL_BUTTON_RIGHT)});
    action.build = actions.add("build", {Binding::key(SDL_SCANCODE_B)});
}

bool option(void* context, std::string_view arg) {
    auto& demo = *static_cast<Demo*>(context);
    if (arg == "--damage-demo")
        demo.damage_demo = true;
    else if (arg == "--overview")
        demo.overview = true;
    else if (arg == "--verify-stream")
        demo.verify_stream = true;
    else
        return false;
    return true;
}

void validate(void* context, const seed::AppOptions& options) {
    if (options.benchmark && static_cast<Demo*>(context)->verify_stream)
        throw std::invalid_argument("Benchmark excludes --verify-stream");
}

// Edit terrain and destroy one platform plank (recipe body 4), stream chunk (0, 0) out and back
// in, and check that both changes came back from disk, including through a background checkpoint.
void verify_stream(seed::Engine& engine, seed::WorldPosition spawn) {
    auto& world = engine.world;
    auto& physics = engine.physics;
    world.settle({});
    dig(world, {{}, {10.5F, 10.5F}});
    const auto plank = physics.find({}, 4);
    if (!plank || !physics.damage(*plank, 100) || physics.find({}, 4))
        throw std::runtime_error("Could not destroy the test plank");
    world.settle({8, 8});
    if (physics.find({}, 5)) throw std::runtime_error("Chunk (0, 0) bodies stayed resident");
    world.settle({});
    const auto* changed = world.tile({{}, {10.5F, 10.5F}});
    if (!changed || changed->material != 0 || changed->elevation >= 0)
        throw std::runtime_error("Chunk delta unload/reload check failed");
    if (physics.find({}, 4) || !physics.find({}, 5))
        throw std::runtime_error("Building delta unload/reload check failed");
    world.settle(spawn.chunk);
    engine.write_deltas(spawn);
    engine.checkpoint.begin_commit(engine.jobs);
    world.settle({8, 8});
    world.settle({});
    const auto* frozen_tile = world.tile({{}, {10.5F, 10.5F}});
    if (!frozen_tile || frozen_tile->elevation >= 0 || physics.find({}, 4) || !physics.find({}, 5))
        throw std::runtime_error("Reload through a background checkpoint lost chunk changes");
    world.settle(spawn.chunk);
    std::puts("Terrain and building deltas survived streaming and background checkpoint reload.");
}

seed::Entity setup(void* context, seed::Engine& engine, seed::WorldPosition spawn) {
    static_cast<Demo*>(context)->spawn = spawn;
    return engine.scene.create({spawn, spawn, 0}, {demo::mat::player, {0.7F, 0.7F}});
}

void loaded(void* context, seed::Engine& engine) {
    auto& demo = *static_cast<Demo*>(context);
    if (demo.verify_stream) verify_stream(engine, demo.spawn);
}

void frame(void* context, seed::Engine& engine, float dt) {
    auto& demo = *static_cast<Demo*>(context);
    demo.damage_cooldown = std::max(0.0F, demo.damage_cooldown - dt);
    if (engine.actions.pressed(demo.action.map)) demo.overview = !demo.overview;
    if (demo.damage_demo && engine.frames == 10) {
        for (std::uint16_t pier = 0; pier < demo::platform_piers; ++pier)
            engine.physics.destroy({}, pier);
        engine.particles.burst({{}, {0, 5}});
        engine.audio.impact();
    }
}

// The player may not walk into water, trees or unloaded ground.
bool blocked(void*, const seed::Tile* tile) {
    return !tile || tile->elevation < 0 || (tile->flags & seed::tile_solid);
}

// Walk with WASD, sliding along water, trees and building pieces.
void step(void* context, seed::Engine& engine, float dt) {
    const auto& action = static_cast<Demo*>(context)->action;
    const auto& actions = engine.actions;
    auto& transform = *engine.scene.transforms.find(engine.focus);
    transform.previous = transform.position;
    const seed::Vec2 movement{actions.axis(action.left, action.right), actions.axis(action.down, action.up)};
    transform.position =
        seed::move_character(engine.world, engine.physics, transform.position,
                             seed::normalized(movement) * (6 * dt), {0.25F, 0.25F}, {nullptr, blocked});
}

float zoom(void* context, seed::Engine&) {
    return static_cast<Demo*>(context)->overview ? 12.0F : 40.0F;
}

// Left mouse damages, right mouse digs, B builds; all within four world units of the player.
void act(void* context, seed::Engine& engine, const seed::View& view) {
    auto& demo = *static_cast<Demo*>(context);
    const auto& actions = engine.actions;
    auto& physics = engine.physics;
    auto& world = engine.world;
    const bool pointing =
        (actions.held(demo.action.damage) || actions.held(demo.action.dig)) && demo.damage_cooldown == 0;
    if (!pointing && !actions.pressed(demo.action.build)) return;
    physics.finish_step();
    const auto target = view.pointer;
    if (seed::length(seed::relative(target, engine.focus_position())) > 4) return;
    if (actions.pressed(demo.action.build)) {
        const auto* ground = world.tile(target);
        if (ground && ground->elevation >= 0 && !(ground->flags & seed::tile_solid)) physics.build(target);
        return;
    }
    bool hit = false;
    if (actions.held(demo.action.dig)) {
        hit = dig(world, target);
        if (hit) physics.damage(target, 100);
    } else
        hit = physics.damage(target, 35) || fell(world, target);
    if (hit) {
        engine.particles.burst(target);
        engine.audio.impact();
    }
    demo.damage_cooldown = 0.15F;
}

void render(void*, seed::Engine& engine, const seed::View& view) {
    auto& renderer = engine.renderer;
    const auto camera = view.camera;
    engine.world.each([&](seed::ChunkCoord coord, const seed::Chunk& chunk) {
        const auto offset = seed::relative({coord, {}}, camera);
        for (int y = 0; y < seed::chunk_side; ++y)
            for (int x = 0; x < seed::chunk_side; ++x) {
                const auto& tile = chunk.tiles[static_cast<std::size_t>(y * seed::chunk_side + x)];
                renderer.sprite(tile.material, offset.x + x + 0.5F, offset.y + y + 0.5F, 1, 1, 0,
                                0.94F + demo::tile_moisture(tile) * 0.15F);
            }
    });
    engine.world.each([&](seed::ChunkCoord coord, const seed::Chunk& chunk) {
        const auto offset = seed::relative({coord, {}}, camera);
        for (int y = 0; y < seed::chunk_side; ++y)
            for (int x = 0; x < seed::chunk_side; ++x) {
                if (!(chunk.tiles[static_cast<std::size_t>(y * seed::chunk_side + x)].flags &
                      seed::tile_solid))
                    continue;
                const float px = offset.x + x + 0.5F, py = offset.y + y + 0.5F;
                renderer.sprite(demo::mat::leaves, px + 0.4F, py - 0.3F, 2.0F, 1.3F, 0, 0.3F);
                renderer.sprite(demo::mat::wood, px, py, 0.35F, 0.65F);
                renderer.sprite(demo::mat::leaves, px, py + 0.5F, 1.8F, 1.8F);
                renderer.sprite(demo::mat::leaves, px - 0.2F, py + 0.9F, 1.1F, 1.1F, 0, 1.3F);
            }
    });
    engine.draw_entities(view);
    engine.particles.draw(renderer, camera, demo::mat::wood);
    if (seed::nearby({}, camera.chunk, 3)) {
        const auto fire = seed::relative({{}, {1, 1}}, camera);
        const float flicker = 1 + 0.08F * std::sin(static_cast<float>(engine.frames) * 0.7F);
        renderer.sprite(demo::mat::ember, fire.x, fire.y, 0.7F * flicker, 1.2F * flicker);
        renderer.light(fire.x, fire.y, 9, 1, 0.53F, 0.19F, 5 * flicker, 2.5F);
    }
    renderer.light(0, 0, 6, 0.6F, 0.72F, 1, 1, 4);
}

// Anchored pieces are stone, everything else timber, drawn at collision size.
seed::Visual body_visual(void*, const seed::BodyState& body) {
    return {body.inverse_mass == 0 ? demo::mat::stone : demo::mat::wood, body.half * 2};
}

void describe(void* context, seed::BenchmarkMetadata& info) {
    const auto& demo = *static_cast<Demo*>(context);
    info.damage_demo = demo.damage_demo;
    info.overview = demo.overview;
}

void shutdown(void* context, seed::Engine& engine) {
    auto& physics = engine.physics;
    if (static_cast<Demo*>(context)->damage_demo && physics.grounded_unsupported() == 0)
        throw std::runtime_error("Collapse check failed: no unsupported pieces reached the ground");
    std::printf("Presented %u frames; %u draw calls; %u unsupported pieces (%u grounded); changes saved.\n",
                engine.frames, engine.renderer.draw_calls(), physics.unsupported(),
                physics.grounded_unsupported());
}
} // namespace

int main(int argc, char** argv) {
    Demo demo;
    seed::Game game;
    game.context = &demo;
    game.id = "seed-demo";
    game.name = "seed_demo";
    game.title = "Seed Engine | WASD | Left damage / Right dig | B build | F5 save | Tab map | Esc save";
    game.asset_pack = "demo.pak";
    game.default_seed = 20260923;
    game.default_save = "saves/island";
    game.usage = "[--damage-demo] [--overview] [--verify-stream] ";
    game.world = demo::world_generator();
    game.materials = demo::register_materials;
    game.body_visual = body_visual;
    game.body_lift_per_height = 0.35F; // Raised timbers draw slightly higher on screen.
    game.actions = register_actions;
    game.option = option;
    game.validate = validate;
    game.setup = setup;
    game.loaded = loaded;
    game.frame = frame;
    game.step = step;
    game.zoom = zoom;
    game.act = act;
    game.render = render;
    game.describe = describe;
    game.shutdown = shutdown;
    return seed::run(game, argc, argv);
}
