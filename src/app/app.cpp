#include "app/app.hpp"
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

Materials register_materials(const Game& game) {
    Materials materials;
    game.materials(game.context, materials);
    return materials;
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
      assets_(std::make_unique<PackStream>(jobs, asset_path(game.asset_pack))),
      materials(register_materials(game)),
      window(!options.smoke && !options.benchmark),
      renderer(assets_->get(), materials),
      gpu(options.benchmark ? options.measured_frames : 0),
      scene(game.scene_memory ? game.scene_memory : Scene::default_memory),
      checkpoint(options.save),
      world(jobs, game.world, options.seed, checkpoint.working_directory(),
            [this](const auto& path) { return checkpoint.read_path(path.filename().string()); }),
      physics(scene, jobs, {game.context, game.body_visual, game.body_lift_per_height}),
      audio(!options.smoke && !options.benchmark),
      measurements_(options.benchmark ? options.measured_frames : 0) {
    if (checkpoint.recovered()) std::puts("Recovered the previous complete checkpoint.");
    world.observe(physics.hooks());
}

Engine::~Engine() = default;

WorldPosition Engine::focus_position() {
    return scene.transforms.find(focus)->position;
}

void Engine::write_deltas(WorldPosition focus_at) {
    physics.finish_step();
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
        if (options.benchmark) {
            const bool quit = input.quit || input.pressed[SDL_SCANCODE_ESCAPE];
            input = {};
            input.quit = quit;
        }
        // Join the step left running during last frame's rendering before touching bodies.
        const auto physics_join_start = MeasurementClock::now();
        physics.finish_step();
        const auto physics_join_end = MeasurementClock::now();
        const auto physics_ns = physics.last_step_nanoseconds();
        const auto frame_bodies = physics.count();
        if (game.frame) game.frame(game.context, *this, dt);
        if (input.pressed[SDL_SCANCODE_ESCAPE]) input.quit = true;
        auto& transform = *scene.transforms.find(focus);
        if (options.benchmark && options.stream_workload) {
            // A fixed route deliberately crosses the unload boundary and returns to the start.
            constexpr ChunkCoord route[] = {{0, 0}, {4, 0}, {4, 4}, {0, 4}, {-4, 4}, {-4, 0}, {0, 0}};
            const auto index = frames < warmup_frames ? 0 : ((frames - warmup_frames) / 60) % 7;
            transform.position = {route[index], {0, 0}};
            transform.previous = transform.position;
        }
        const auto stream_start = MeasurementClock::now();
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
        if (game.render) game.render(game.context, *this, view);
        renderer.finish();
        gpu.end();
        const auto render_end = MeasurementClock::now();
        if (options.screenshot && ((options.smoke && frames == 59) || input.pressed[SDL_SCANCODE_F12]))
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
            (input.pressed[SDL_SCANCODE_F5] || milliseconds(last_save, frame_end) >= 60000)) {
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
        if (!game.setup || !game.materials || !game.name || !game.asset_pack || !game.default_save)
            throw std::invalid_argument(
                "A game needs a name, an asset pack, a default save, materials() and setup()");
        const auto options = parse(game, argc, argv);
        Engine engine(game, options);
        const auto spawn = load_player(engine.checkpoint.read_path("player.delta").parent_path(),
                                       options.seed, engine.world.generator().version);
        engine.focus = game.setup(game.context, engine, spawn);
        if (!engine.scene.transforms.find(engine.focus))
            throw std::logic_error("setup() must return an entity with a Transform");
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
