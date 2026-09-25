// The demo game: explore a procedural island, damage a timber platform, dig and build.
#include "app/app.hpp"
#include "world/generator.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string_view>

namespace {
struct Demo {
    bool damage_demo{}, overview{}, verify_stream{};
    float damage_cooldown{};
    seed::WorldPosition spawn;
};

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
    world.dig({{}, {10.5F, 10.5F}});
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
    return engine.scene.create({spawn, spawn, 0}, {});
}

void loaded(void* context, seed::Engine& engine) {
    auto& demo = *static_cast<Demo*>(context);
    if (demo.verify_stream) verify_stream(engine, demo.spawn);
}

void frame(void* context, seed::Engine& engine, float dt) {
    auto& demo = *static_cast<Demo*>(context);
    demo.damage_cooldown = std::max(0.0F, demo.damage_cooldown - dt);
    if (engine.input.pressed[SDL_SCANCODE_TAB]) demo.overview = !demo.overview;
    if (demo.damage_demo && engine.frames == 10) {
        engine.physics.collapse_demo();
        engine.particles.burst({{}, {0, 5}});
        engine.audio.impact();
    }
}

// Walk with WASD; water, trees and building pieces block movement.
void step(void*, seed::Engine& engine, float dt) {
    auto& input = engine.input;
    auto& transform = *engine.scene.transforms.find(engine.focus);
    transform.previous = transform.position;
    const seed::Vec2 movement{
        static_cast<float>(input.held[SDL_SCANCODE_D]) - static_cast<float>(input.held[SDL_SCANCODE_A]),
        static_cast<float>(input.held[SDL_SCANCODE_W]) - static_cast<float>(input.held[SDL_SCANCODE_S])};
    auto candidate = transform.position;
    candidate.move(seed::normalized(movement) * (6 * dt));
    const auto* ground = engine.world.tile(candidate);
    if (ground && ground->elevation >= 0 && !ground->tree && !engine.physics.blocks(candidate))
        transform.position = candidate;
}

float zoom(void* context, seed::Engine&) {
    return static_cast<Demo*>(context)->overview ? 12.0F : 40.0F;
}

// Left mouse damages, right mouse digs, B builds; all within four world units of the player.
void act(void* context, seed::Engine& engine, const seed::View& view) {
    auto& demo = *static_cast<Demo*>(context);
    auto& input = engine.input;
    auto& physics = engine.physics;
    auto& world = engine.world;
    const bool pointing =
        (input.mouse_buttons & (SDL_BUTTON_LMASK | SDL_BUTTON_RMASK)) && demo.damage_cooldown == 0;
    if (!pointing && !input.pressed[SDL_SCANCODE_B]) return;
    physics.finish_step();
    const auto target = view.pointer;
    if (seed::length(seed::relative(target, engine.focus_position())) > 4) return;
    if (input.pressed[SDL_SCANCODE_B]) {
        const auto* ground = world.tile(target);
        if (ground && ground->elevation >= 0 && !ground->tree) physics.build(target);
        return;
    }
    bool hit = false;
    if (input.mouse_buttons & SDL_BUTTON_RMASK) {
        hit = world.dig(target);
        if (hit) physics.damage(target, 100);
    } else
        hit = physics.damage(target, 35) || world.remove_tree(target);
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
                renderer.sprite(static_cast<seed::Material>(tile.material), offset.x + x + 0.5F,
                                offset.y + y + 0.5F, 1, 1, 0, 0.94F + tile.moisture * 0.15F);
            }
    });
    engine.world.each([&](seed::ChunkCoord coord, const seed::Chunk& chunk) {
        const auto offset = seed::relative({coord, {}}, camera);
        for (int y = 0; y < seed::chunk_side; ++y)
            for (int x = 0; x < seed::chunk_side; ++x) {
                if (!chunk.tiles[static_cast<std::size_t>(y * seed::chunk_side + x)].tree) continue;
                const float px = offset.x + x + 0.5F, py = offset.y + y + 0.5F;
                renderer.sprite(seed::Material::leaves, px + 0.4F, py - 0.3F, 2.0F, 1.3F, 0, 0.3F);
                renderer.sprite(seed::Material::wood, px, py, 0.35F, 0.65F);
                renderer.sprite(seed::Material::leaves, px, py + 0.5F, 1.8F, 1.8F);
                renderer.sprite(seed::Material::leaves, px - 0.2F, py + 0.9F, 1.1F, 1.1F, 0, 1.3F);
            }
    });
    engine.draw_entities(view);
    engine.particles.draw(renderer, camera);
    if (seed::nearby({}, camera.chunk, 3)) {
        const auto fire = seed::relative({{}, {1, 1}}, camera);
        const float flicker = 1 + 0.08F * std::sin(static_cast<float>(engine.frames) * 0.7F);
        renderer.sprite(seed::Material::ember, fire.x, fire.y, 0.7F * flicker, 1.2F * flicker);
        renderer.light(fire.x, fire.y, 9, 1, 0.53F, 0.19F, 5 * flicker, 2.5F);
    }
    renderer.light(0, 0, 6, 0.6F, 0.72F, 1, 1, 4);
}

void describe(void* context, seed::BenchmarkMetadata& info) {
    const auto& demo = *static_cast<Demo*>(context);
    info.generator_version = seed::generator_version;
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
    game.name = "seed_demo";
    game.title = "Seed Engine | WASD | Left damage / Right dig | B build | F5 save | Tab map | Esc save";
    game.asset_pack = "demo.pak";
    game.default_seed = 20260923;
    game.default_save = "saves/island";
    game.usage = "[--damage-demo] [--overview] [--verify-stream] ";
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
