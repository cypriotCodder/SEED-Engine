#include "app/app.hpp"
#include "world/chunk_file.hpp"
#include "world/player_save.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <exception>
#include <string>
#include <thread>

namespace seed {
namespace {
std::filesystem::path asset_path(const char* name) {
    char* base = SDL_GetBasePath();
    if (!base) throw std::runtime_error("Cannot determine executable asset directory");
    const auto path = std::filesystem::path(base) / name;
    SDL_free(base);
    return path;
}

Assets load_project_assets(const Game& game) {
    Assets assets;
    if (game.assets)
        assets = *game.assets;
    else if (game.project_assets) {
        std::filesystem::path folder = game.project_assets;
        if (folder.is_relative()) folder = asset_path(game.project_assets);
        if (!std::filesystem::is_directory(folder))
            throw std::runtime_error("Project assets folder not found: " + folder.string());
        assets = load_assets(folder);
    } else
        return {};
    if (const auto problems = assets.problems(); !problems.empty())
        throw std::runtime_error("The project's assets have problems:\n" + problems);
    return assets;
}

Materials register_materials(const Game& game, const Assets& assets) {
    Materials materials;
    assets.register_materials(materials);
    if (game.materials) game.materials(game.context, materials);
    if (!materials.size()) throw std::invalid_argument("A game needs at least one material");
    return materials;
}

Sounds register_effects(const Game& game, const Assets& assets, const Materials& materials,
                        Particles& particles) {
    Sounds sounds;
    assets.register_effects(materials, sounds, particles);
    if (game.effects) game.effects(game.context, sounds, particles);
    return sounds;
}

// The game's own generator, or one built from the project's terrain.json.
WorldGenerator world_generator(const Game& game, const Assets& assets, const Materials& materials,
                               std::unique_ptr<Terrain>& terrain) {
    if (game.world.terrain) return game.world;
    if (!game.project_assets && !game.assets)
        throw std::invalid_argument("A game needs world.terrain or project assets with a terrain");
    terrain = std::make_unique<Terrain>(assets.terrain, materials);
    return terrain->generator();
}

const Pack& no_assets() {
    static const Pack empty;
    return empty;
}

AppOptions parse(const Game& game, int argc, char** argv) {
    AppOptions options;
    options.seed = game.default_seed;
    options.save = game.default_save;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--smoke")
            options.smoke = true;
        else if (arg == "--screenshot" && i + 1 < argc)
            options.screenshot = argv[++i];
        else if (arg == "--save" && i + 1 < argc) {
            options.save = argv[++i];
            options.explicit_save = true;
        } else if (arg == "--benchmark" && i + 1 < argc)
            options.benchmark = argv[++i];
        else if (arg == "--workload" && i + 1 < argc) {
            const std::string_view value = argv[++i];
            if (value != "static" && value != "stream") throw std::invalid_argument("Unknown workload");
            options.stream_workload = value == "stream";
        } else if (arg == "--frames" && i + 1 < argc) {
            const std::string_view value = argv[++i];
            const auto r =
                std::from_chars(value.data(), value.data() + value.size(), options.measured_frames);
            if (r.ec != std::errc{} || r.ptr != value.data() + value.size() || !options.measured_frames ||
                options.measured_frames > 10000)
                throw std::invalid_argument("Benchmark frames must be between 1 and 10000");
        } else if (arg == "--seed" && i + 1 < argc) {
            const std::string_view value = argv[++i];
            const auto result = std::from_chars(value.data(), value.data() + value.size(), options.seed);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size())
                throw std::invalid_argument("Invalid 64-bit seed");
        } else if (!game.option || !game.option(game.context, arg))
            throw std::invalid_argument(std::string("Usage: ") + game.name + " [--smoke] " +
                                        (game.usage ? game.usage : "") +
                                        "[--screenshot FILE.ppm] [--seed N] [--save DIRECTORY] "
                                        "[--benchmark REPORT.json --frames N --workload static|stream]");
    }
    if (game.validate) game.validate(game.context, options);
    if (options.benchmark && (!options.explicit_save || options.screenshot || options.smoke))
        throw std::invalid_argument("Benchmark requires --save and excludes smoke and screenshot");
    if (options.benchmark && std::filesystem::exists(options.save) &&
        !std::filesystem::is_empty(options.save))
        throw std::invalid_argument(
            "Benchmark requires an empty save directory for repeatable initial state");
    return options;
}
} // namespace

Engine::Engine(const Game& game, const AppOptions& opts)
    : options(opts),
      assets_(game.asset_pack ? std::make_unique<PackStream>(jobs, asset_path(game.asset_pack)) : nullptr),
      project_assets_(load_project_assets(game)),
      materials(register_materials(game, project_assets_)),
      window(!options.smoke && !options.benchmark),
      renderer(assets_ ? assets_->get() : no_assets(), materials),
      gpu(options.benchmark ? options.measured_frames : 0),
      scene(game.scene_memory ? game.scene_memory : Scene::default_memory),
      checkpoint(options.save),
      world(jobs, stable_id(game.id), world_generator(game, project_assets_, materials, terrain_),
            options.seed, checkpoint.working_directory(),
            [this](const auto& path) { return checkpoint.read_path(path.filename().string()); }),
      physics(scene, jobs, {game.context, game.body_visual, game.body_lift_per_height}),
      sounds(register_effects(game, project_assets_, materials, particles)),
      audio(!options.smoke && !options.benchmark, sounds),
      measurements_(options.benchmark ? options.measured_frames : 0),
      physics_hooks_(physics.hooks()),
      game_context_(game.context),
      save_entity_(game.save_entity),
      load_entity_(game.load_entity) {
    if (checkpoint.recovered()) std::puts("Recovered the previous complete checkpoint.");
    add_engine_actions(actions);
    project_assets_.register_actions(actions);
    if (game.actions) game.actions(game.context, actions);
    ChunkHooks hooks;
    hooks.context = this;
    hooks.activate = [](void* context, ChunkCoord coord, Chunk& chunk) {
        auto& engine = *static_cast<Engine*>(context);
        engine.physics_hooks_.activate(engine.physics_hooks_.context, coord, chunk);
        engine.restore(coord, chunk);
    };
    hooks.release = [](void* context, ChunkCoord coord, Chunk& chunk) {
        auto& engine = *static_cast<Engine*>(context);
        if (engine.capture(coord, chunk, true)) chunk.dirty = true;
        engine.physics_hooks_.release(engine.physics_hooks_.context, coord, chunk);
    };
    hooks.store = [](void* context, ChunkCoord coord, Chunk& chunk) {
        auto& engine = *static_cast<Engine*>(context);
        if (engine.capture(coord, chunk, false)) chunk.dirty = true;
        engine.physics_hooks_.store(engine.physics_hooks_.context, coord, chunk);
    };
    world.observe(hooks);
}

Entity Engine::create_saved(Transform transform, Visual visual) {
    transform.position.move({});
    const auto entity = scene.create(transform, visual);
    scene.saved.add(entity, {transform.position.chunk});
    return entity;
}

void Engine::update_owners() {
    const auto owners = scene.saved.owners();
    const auto saved = scene.saved.values();
    for (std::size_t i = 0; i < owners.size(); ++i) {
        auto position = scene.transforms.find(owners[i])->position;
        position.move({});
        if (world.active(position.chunk)) saved[i].owner = position.chunk;
    }
}

void Engine::restore(ChunkCoord coord, Chunk& chunk) {
    each_entity(chunk.entities, [&](const EntityRecord& record) {
        static_cast<void>(materials[record.material]); // Rejects unregistered materials.
        const auto entity =
            scene.create({record.position, record.position, record.angle}, {record.material, record.size});
        scene.saved.add(entity, {coord});
        Reader in(record.payload);
        if (load_entity_) load_entity_(game_context_, *this, entity, in);
        if (!in.done()) throw std::runtime_error("The game did not read a saved entity's whole payload");
    });
}

bool Engine::capture(ChunkCoord coord, Chunk& chunk, bool remove) {
    captured_.clear();
    const auto owners = scene.saved.owners();
    const auto saved = scene.saved.values();
    for (std::size_t i = 0; i < owners.size(); ++i)
        if (saved[i].owner == coord) captured_.push_back(owners[i]);
    ChunkEntities records;
    Bytes payload;
    for (const auto entity : captured_) {
        if (entity == focus) throw std::logic_error("The focus entity cannot be saved with a chunk");
        const auto& transform = *scene.transforms.find(entity);
        const auto* visual = scene.visuals.find(entity);
        payload.data.clear();
        if (save_entity_) save_entity_(game_context_, *this, entity, payload);
        append_entity(records, {transform.position, transform.angle, visual ? visual->material : MaterialId{},
                                visual ? visual->size : Vec2{}, payload.data});
    }
    if (remove)
        for (const auto entity : captured_)
            scene.destroy(entity);
    if (records == chunk.entities) return false;
    chunk.entities = std::move(records);
    return true;
}

Engine::~Engine() = default;

WorldPosition Engine::focus_position() {
    return scene.transforms.find(focus)->position;
}

void Engine::write_deltas(WorldPosition focus_at) {
    physics.finish_step();
    update_owners();
    world.save();
    save_player(checkpoint.working_directory(), options.seed, world.generator().version, focus_at,
                std::filesystem::exists(checkpoint.read_path("player.delta")));
}

void Engine::draw_entities(const View& view) {
    const auto owners = scene.visuals.owners();
    const auto visuals = scene.visuals.values();
    for (std::size_t i = 0; i < owners.size(); ++i) {
        const auto& t = *scene.transforms.find(owners[i]);
        if (!nearby(t.position.chunk, view.camera.chunk, 3)) continue;
        const auto p = relative(t.previous, view.camera) + relative(t.position, t.previous) * view.alpha;
        renderer.sprite(visuals[i].material, p.x, p.y, visuals[i].size.x, visuals[i].size.y, t.angle);
    }
}

void Engine::loop(const Game& game) {
    constexpr unsigned warmup_frames = AppOptions::warmup_frames;
    const bool automated = options.smoke || options.benchmark;
    auto previous = std::chrono::steady_clock::now();
    float accumulator = 0;
    auto last_save = MeasurementClock::now();
    int report_width = 0, report_height = 0;
    const auto save_checkpoint = [&](bool background = false) {
        write_deltas(focus_position());
        if (background)
            checkpoint.begin_commit(jobs);
        else
            checkpoint.commit();
    };
    while (!input.quit) {
        if (checkpoint.finish_ready()) std::puts("Checkpoint saved.");
        const auto now = std::chrono::steady_clock::now();
        const float dt = automated
                             ? 1.0F / 60.0F
                             : std::clamp(std::chrono::duration<float>(now - previous).count(), 0.0F, 0.1F);
        previous = now;
        window.poll(input);
        actions.update(input);
        if (options.benchmark) {
            // Benchmarks ignore every input except quitting.
            const bool quit = input.quit || actions.pressed(engine_action::quit);
            input = {};
            input.quit = quit;
            actions.update(input);
        }
        // Join the step left running during last frame's rendering before touching bodies.
        const auto physics_join_start = MeasurementClock::now();
        physics.finish_step();
        const auto physics_join_end = MeasurementClock::now();
        const auto physics_ns = physics.last_step_nanoseconds();
        const auto frame_bodies = physics.count();
        if (game.frame) game.frame(game.context, *this, dt);
        if (actions.pressed(engine_action::quit)) input.quit = true;
        auto& transform = *scene.transforms.find(focus);
        if (options.benchmark && options.stream_workload) {
            // A fixed route deliberately crosses the unload boundary and returns to the start.
            constexpr ChunkCoord route[] = {{0, 0}, {4, 0}, {4, 4}, {0, 4}, {-4, 4}, {-4, 0}, {0, 0}};
            const auto index = frames < warmup_frames ? 0 : ((frames - warmup_frames) / 60) % 7;
            transform.position = {route[index], {0, 0}};
            transform.previous = transform.position;
        }
        const auto stream_start = MeasurementClock::now();
        update_owners();
        world.stream(transform.position.chunk);
        const auto stream_end = MeasurementClock::now();
        accumulator += dt;
        constexpr float step = 1.0F / 60.0F;
        while (accumulator >= step) {
            physics.finish_step();
            if (game.step) game.step(game.context, *this, step);
            // The last step of the frame keeps running while the frame renders.
            physics.begin_step(transform.position);
            particles.update(step);
            accumulator -= step;
        }
        View view;
        view.camera = transform.position;
        view.camera.move(relative(transform.previous, transform.position) * (1 - accumulator / step));
        view.alpha = accumulator / step;
        window.drawable_size(view.width, view.height);
        window.logical_size(view.logical_width, view.logical_height);
        if (view.width <= 0 || view.height <= 0 || view.logical_width <= 0 || view.logical_height <= 0) {
            SDL_Delay(16);
            continue;
        }
        view.zoom = (game.zoom ? game.zoom(game.context, *this) : 40.0F) * static_cast<float>(view.width) /
                    view.logical_width;
        view.pointer = view.camera;
        view.pointer.move(
            {(static_cast<float>(input.mouse_x) / view.logical_width - 0.5F) * view.width / view.zoom,
             (0.5F - static_cast<float>(input.mouse_y) / view.logical_height) * view.height / view.zoom});
        if (game.act) game.act(game.context, *this, view);

        const auto render_start = MeasurementClock::now();
        const bool record = options.benchmark && frames >= warmup_frames;
        gpu.begin(record);
        renderer.begin(view.width, view.height, 0, 0, view.zoom);
        renderer.set_ui_scale(static_cast<float>(view.width) / static_cast<float>(view.logical_width));
        if (game.render) game.render(game.context, *this, view);
        renderer.finish();
        gpu.end();
        const auto render_end = MeasurementClock::now();
        if (options.screenshot &&
            ((options.smoke && frames == 59) || actions.pressed(engine_action::screenshot)))
            renderer.screenshot(options.screenshot, view.width, view.height);
        window.present();
        const auto frame_end = MeasurementClock::now();
        if (record) {
            measurements_.frame_times.add(milliseconds(now, frame_end));
            measurements_.update_times.add(milliseconds(now, render_start));
            measurements_.stream_times.add(milliseconds(stream_start, stream_end));
            measurements_.render_times.add(milliseconds(render_start, render_end));
            measurements_.present_times.add(milliseconds(render_end, frame_end));
            measurements_.physics_times.add(physics_ns / 1000000.0);
            measurements_.physics_join_times.add(milliseconds(physics_join_start, physics_join_end));
            measurements_.draws.add(renderer.draw_calls());
            measurements_.bodies.add(static_cast<double>(frame_bodies));
            // The worker owns body state until the next frame; capture counts only after joining.
            report_width = view.width;
            report_height = view.height;
        }
        if (frames % 60 == 0) {
            char title[200];
            std::snprintf(title, sizeof(title), "%s | %u draws", game.title ? game.title : game.name,
                          renderer.draw_calls());
            window.title(title);
        }
        ++frames;
        if (options.smoke && frames >= 60) input.quit = true;
        if (options.benchmark && frames >= options.measured_frames + warmup_frames) input.quit = true;
        if (!automated && !checkpoint.saving() &&
            (actions.pressed(engine_action::checkpoint) || milliseconds(last_save, frame_end) >= 60000)) {
            save_checkpoint(true);
            last_save = MeasurementClock::now();
        }
        if (!automated) std::this_thread::sleep_until(previous + std::chrono::microseconds(16667));
    }
    gpu.drain();
    const auto save_start = MeasurementClock::now();
    save_checkpoint();
    const auto save_ms = milliseconds(save_start, MeasurementClock::now());
    if (options.benchmark) {
        if (frames != options.measured_frames + warmup_frames)
            throw std::runtime_error("Benchmark interrupted before all frames were measured");
        const auto generation = world.generation_metrics();
        BenchmarkMetadata info{options.seed,
                               world.generator().version,
                               warmup_frames,
                               options.measured_frames,
                               report_width,
                               report_height,
                               options.stream_workload,
                               false,
                               false,
                               SDL_GetPlatform(),
                               gpu.renderer(),
                               gpu.version(),
                               generation.chunks,
                               generation.nanoseconds,
                               generation.maximum_nanoseconds,
                               save_ms,
                               options.save};
        if (game.describe) game.describe(game.context, info);
        measurements_.write(options.benchmark, info, gpu.samples, gpu.skipped);
    }
}

int run(const Game& game, int argc, char** argv) {
    try {
        if (!game.setup || !game.id || !game.name || !game.default_save)
            throw std::invalid_argument("A game needs an id, a name, a default save and setup()");
        if (!game.save_entity != !game.load_entity)
            throw std::invalid_argument("save_entity() and load_entity() must be given together");
        const auto options = parse(game, argc, argv);
        Engine engine(game, options);
        const auto spawn = load_player(engine.checkpoint.read_path("player.delta").parent_path(),
                                       options.seed, engine.world.generator().version);
        engine.focus = game.setup(game.context, engine, spawn);
        if (!engine.scene.transforms.find(engine.focus))
            throw std::logic_error("setup() must return an entity with a Transform");
        if (engine.scene.saved.find(engine.focus))
            throw std::logic_error("setup() must not return a saved entity; the focus is saved separately");
        engine.world.settle(spawn.chunk);
        if (game.loaded) game.loaded(game.context, engine);
        engine.loop(game);
        if (game.shutdown) game.shutdown(game.context, engine);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Engine error: %s\n", error.what());
        return 1;
    }
}
} // namespace seed
