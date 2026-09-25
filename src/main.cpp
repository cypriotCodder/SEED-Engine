#include "core/metrics.hpp"
#include "core/particles.hpp"
#include "core/scene.hpp"
#include "io/checkpoint.hpp"
#include "physics/physics.hpp"
#include "platform/audio.hpp"
#include "platform/window.hpp"
#include "render/gpu_timer.hpp"
#include "render/renderer.hpp"
#include "world/player_save.hpp"
#include "world/world.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string_view>

int main(int argc, char** argv) {
    try {
        bool smoke = false;
        bool damage_demo = false, overview = false;
        bool verify_stream = false;
        const char* screenshot = nullptr;
        const char* benchmark = nullptr;
        unsigned measured_frames = 600;
        constexpr unsigned warmup_frames = 60;
        bool stream_workload = false, explicit_save = false;
        std::uint64_t seed = 20260923;
        std::filesystem::path save = "saves/island";
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i];
            if (arg == "--smoke")
                smoke = true;
            else if (arg == "--damage-demo")
                damage_demo = true;
            else if (arg == "--overview")
                overview = true;
            else if (arg == "--verify-stream")
                verify_stream = true;
            else if (arg == "--screenshot" && i + 1 < argc)
                screenshot = argv[++i];
            else if (arg == "--save" && i + 1 < argc) {
                save = argv[++i];
                explicit_save = true;
            } else if (arg == "--benchmark" && i + 1 < argc)
                benchmark = argv[++i];
            else if (arg == "--workload" && i + 1 < argc) {
                const std::string_view value = argv[++i];
                if (value != "static" && value != "stream") throw std::invalid_argument("Unknown workload");
                stream_workload = value == "stream";
            } else if (arg == "--frames" && i + 1 < argc) {
                const std::string_view value = argv[++i];
                const auto r = std::from_chars(value.data(), value.data() + value.size(), measured_frames);
                if (r.ec != std::errc{} || r.ptr != value.data() + value.size() || !measured_frames ||
                    measured_frames > 10000)
                    throw std::invalid_argument("Benchmark frames must be between 1 and 10000");
            } else if (arg == "--seed" && i + 1 < argc) {
                const std::string_view value = argv[++i];
                const auto result = std::from_chars(value.data(), value.data() + value.size(), seed);
                if (result.ec != std::errc{} || result.ptr != value.data() + value.size())
                    throw std::invalid_argument("Invalid 64-bit seed");
            } else
                throw std::invalid_argument("Usage: seed_demo [--smoke] [--damage-demo] [--overview] "
                                            "[--screenshot FILE.ppm] [--seed N] [--save DIRECTORY] "
                                            "[--benchmark REPORT.json --frames N --workload static|stream]");
        }
        if (benchmark && (!explicit_save || screenshot || smoke || verify_stream))
            throw std::invalid_argument(
                "Benchmark requires --save and excludes smoke, screenshot and verify-stream");
        if (benchmark && std::filesystem::exists(save) && !std::filesystem::is_empty(save))
            throw std::invalid_argument(
                "Benchmark requires an empty save directory for repeatable initial state");
        seed::BenchmarkReport measurements(benchmark ? measured_frames : 0);
        seed::Jobs jobs;
        char* base = SDL_GetBasePath();
        if (!base) throw std::runtime_error("Cannot determine executable asset directory");
        const auto pack_path = std::filesystem::path(base) / "demo.pak";
        SDL_free(base);
        seed::PackStream assets(jobs, pack_path);
        seed::Window window(!smoke && !benchmark);
        seed::Renderer renderer(assets.get());
        seed::GpuTimer gpu(benchmark ? measured_frames : 0);
        seed::Scene scene;
        seed::Checkpoint checkpoint(save);
        if (checkpoint.recovered()) std::puts("Recovered the previous complete checkpoint.");
        const auto& working_save = checkpoint.working_directory();
        seed::World world(jobs, seed, working_save,
                          [&](const auto& path) { return checkpoint.read_path(path.filename().string()); });
        seed::Physics physics(scene, jobs);
        world.observe(physics.hooks());
        const auto spawn = seed::load_player(checkpoint.read_path("player.delta").parent_path(), seed);
        const auto player = scene.create({spawn, spawn, 0}, {});
        world.settle(spawn.chunk);
        if (verify_stream) {
            // Edit terrain and destroy one platform plank (recipe body 4), stream chunk (0, 0) out
            // and back in, and check that both changes came back from disk.
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
            world.save();
            seed::save_player(working_save, seed, spawn,
                              std::filesystem::exists(checkpoint.read_path("player.delta")));
            checkpoint.begin_commit(jobs);
            world.settle({8, 8});
            world.settle({});
            const auto* frozen_tile = world.tile({{}, {10.5F, 10.5F}});
            if (!frozen_tile || frozen_tile->elevation >= 0 || physics.find({}, 4) || !physics.find({}, 5))
                throw std::runtime_error("Reload through a background checkpoint lost chunk changes");
            world.settle(spawn.chunk);
            std::puts("Terrain and building deltas survived streaming and background checkpoint reload.");
        }
        seed::Particles particles;
        seed::Audio audio(!smoke && !benchmark);
        seed::Input input;
        auto previous = std::chrono::steady_clock::now();
        float accumulator = 0;
        unsigned frames = 0;
        float damage_cooldown = 0;
        auto last_save = seed::MeasurementClock::now();
        int report_width = 0, report_height = 0;
        const auto save_checkpoint = [&](bool background = false) {
            physics.finish_step();
            world.save();
            seed::save_player(working_save, seed, scene.transforms.find(player)->position,
                              std::filesystem::exists(checkpoint.read_path("player.delta")));
            if (background)
                checkpoint.begin_commit(jobs);
            else
                checkpoint.commit();
        };
        while (!input.quit) {
            if (checkpoint.finish_ready()) std::puts("Checkpoint saved.");
            const auto now = std::chrono::steady_clock::now();
            const float dt =
                (smoke || benchmark)
                    ? 1.0F / 60.0F
                    : std::clamp(std::chrono::duration<float>(now - previous).count(), 0.0F, 0.1F);
            previous = now;
            window.poll(input);
            if (benchmark) {
                const bool quit = input.quit || input.pressed[SDL_SCANCODE_ESCAPE];
                input = {};
                input.quit = quit;
            }
            // Join the step left running during last frame's rendering before touching bodies.
            const auto physics_join_start = seed::MeasurementClock::now();
            physics.finish_step();
            const auto physics_join_end = seed::MeasurementClock::now();
            const auto physics_ns = physics.last_step_nanoseconds();
            const auto frame_bodies = physics.count();
            damage_cooldown = std::max(0.0F, damage_cooldown - dt);
            if (input.pressed[SDL_SCANCODE_TAB]) overview = !overview;
            if (damage_demo && frames == 10) {
                physics.collapse_demo();
                particles.burst({{}, {0, 5}});
                audio.impact();
            }
            if (input.pressed[SDL_SCANCODE_ESCAPE]) input.quit = true;
            auto& transform = *scene.transforms.find(player);
            if (benchmark && stream_workload) {
                // A fixed route deliberately crosses the unload boundary and returns to the island.
                constexpr seed::ChunkCoord route[] = {{0, 0},  {4, 0},  {4, 4}, {0, 4},
                                                      {-4, 4}, {-4, 0}, {0, 0}};
                const auto index = frames < warmup_frames ? 0 : ((frames - warmup_frames) / 60) % 7;
                transform.position = {route[index], {0, 0}};
                transform.previous = transform.position;
            }
            const auto stream_start = seed::MeasurementClock::now();
            world.stream(transform.position.chunk);
            const auto stream_end = seed::MeasurementClock::now();
            accumulator += dt;
            constexpr float step = 1.0F / 60.0F;
            while (accumulator >= step) {
                physics.finish_step();
                transform.previous = transform.position;
                const seed::Vec2 movement{static_cast<float>(input.held[SDL_SCANCODE_D]) -
                                              static_cast<float>(input.held[SDL_SCANCODE_A]),
                                          static_cast<float>(input.held[SDL_SCANCODE_W]) -
                                              static_cast<float>(input.held[SDL_SCANCODE_S])};
                auto candidate = transform.position;
                candidate.move(seed::normalized(movement) * (6 * step));
                const auto* ground = world.tile(candidate);
                if (ground && ground->elevation >= 0 && !ground->tree && !physics.blocks(candidate))
                    transform.position = candidate;
                // The last step of the frame keeps running while the frame renders.
                physics.begin_step(transform.position);
                particles.update(step);
                accumulator -= step;
            }
            auto camera = transform.position;
            camera.move(seed::relative(transform.previous, transform.position) * (1 - accumulator / step));
            int width{}, height{}, logical_width{}, logical_height{};
            window.drawable_size(width, height);
            window.logical_size(logical_width, logical_height);
            if (width <= 0 || height <= 0 || logical_width <= 0 || logical_height <= 0) {
                SDL_Delay(16);
                continue;
            }
            const float zoom = (overview ? 12.0F : 40.0F) * static_cast<float>(width) / logical_width;
            if (((input.mouse_buttons & (SDL_BUTTON_LMASK | SDL_BUTTON_RMASK)) && damage_cooldown == 0) ||
                input.pressed[SDL_SCANCODE_B]) {
                physics.finish_step();
                auto target = camera;
                target.move({(static_cast<float>(input.mouse_x) / logical_width - 0.5F) * width / zoom,
                             (0.5F - static_cast<float>(input.mouse_y) / logical_height) * height / zoom});
                if (seed::length(seed::relative(target, transform.position)) <= 4) {
                    if (input.pressed[SDL_SCANCODE_B]) {
                        const auto* ground = world.tile(target);
                        if (ground && ground->elevation >= 0 && !ground->tree) physics.build(target);
                    } else {
                        bool hit = false;
                        if (input.mouse_buttons & SDL_BUTTON_RMASK) {
                            hit = world.dig(target);
                            if (hit) physics.damage(target, 100);
                        } else
                            hit = physics.damage(target, 35) || world.remove_tree(target);
                        if (hit) {
                            particles.burst(target);
                            audio.impact();
                        }
                        damage_cooldown = 0.15F;
                    }
                }
            }
            const auto render_start = seed::MeasurementClock::now();
            const bool record = benchmark && frames >= warmup_frames;
            gpu.begin(record);
            renderer.begin(width, height, 0, 0, zoom);
            world.each([&](seed::ChunkCoord coord, const seed::Chunk& chunk) {
                const auto offset = seed::relative({coord, {}}, camera);
                for (int y = 0; y < seed::chunk_side; ++y)
                    for (int x = 0; x < seed::chunk_side; ++x) {
                        const auto& tile = chunk.tiles[static_cast<std::size_t>(y * seed::chunk_side + x)];
                        renderer.sprite(static_cast<seed::Material>(tile.material), offset.x + x + 0.5F,
                                        offset.y + y + 0.5F, 1, 1, 0, 0.94F + tile.moisture * 0.15F);
                    }
            });
            world.each([&](seed::ChunkCoord coord, const seed::Chunk& chunk) {
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
            const auto owners = scene.visuals.owners();
            const auto visuals = scene.visuals.values();
            for (std::size_t i = 0; i < owners.size(); ++i) {
                const auto& t = *scene.transforms.find(owners[i]);
                if (!seed::nearby(t.position.chunk, camera.chunk, 3)) continue;
                const auto p = seed::relative(t.previous, camera) +
                               seed::relative(t.position, t.previous) * (accumulator / step);
                renderer.sprite(visuals[i].material, p.x, p.y, visuals[i].size.x, visuals[i].size.y, t.angle);
            }
            particles.draw(renderer, camera);
            if (seed::nearby({}, camera.chunk, 3)) {
                const auto fire = seed::relative({{}, {1, 1}}, camera);
                const float flicker = 1 + 0.08F * std::sin(static_cast<float>(frames) * 0.7F);
                renderer.sprite(seed::Material::ember, fire.x, fire.y, 0.7F * flicker, 1.2F * flicker);
                renderer.light(fire.x, fire.y, 9, 1, 0.53F, 0.19F, 5 * flicker, 2.5F);
            }
            renderer.light(0, 0, 6, 0.6F, 0.72F, 1, 1, 4);
            renderer.finish();
            gpu.end();
            const auto render_end = seed::MeasurementClock::now();
            if (screenshot && ((smoke && frames == 59) || input.pressed[SDL_SCANCODE_F12]))
                renderer.screenshot(screenshot, width, height);
            window.present();
            const auto frame_end = seed::MeasurementClock::now();
            if (record) {
                measurements.frame_times.add(seed::milliseconds(now, frame_end));
                measurements.update_times.add(seed::milliseconds(now, render_start));
                measurements.stream_times.add(seed::milliseconds(stream_start, stream_end));
                measurements.render_times.add(seed::milliseconds(render_start, render_end));
                measurements.present_times.add(seed::milliseconds(render_end, frame_end));
                measurements.physics_times.add(physics_ns / 1000000.0);
                measurements.physics_join_times.add(seed::milliseconds(physics_join_start, physics_join_end));
                measurements.draws.add(renderer.draw_calls());
                measurements.bodies.add(static_cast<double>(frame_bodies));
                // The worker owns body state until the next frame; capture counts only after joining.
                report_width = width;
                report_height = height;
            }
            if (frames % 60 == 0) {
                char title[160];
                std::snprintf(title, sizeof(title),
                              "Seed Engine | WASD | Left damage / Right dig | B build | F5 save | Tab map | "
                              "Esc save | %u draws",
                              renderer.draw_calls());
                window.title(title);
            }
            ++frames;
            if (smoke && frames >= 60) input.quit = true;
            if (benchmark && frames >= measured_frames + warmup_frames) input.quit = true;
            if (!smoke && !benchmark && !checkpoint.saving() &&
                (input.pressed[SDL_SCANCODE_F5] || seed::milliseconds(last_save, frame_end) >= 60000)) {
                save_checkpoint(true);
                last_save = seed::MeasurementClock::now();
            }
            if (!smoke && !benchmark)
                std::this_thread::sleep_until(previous + std::chrono::microseconds(16667));
        }
        gpu.drain();
        const auto save_start = seed::MeasurementClock::now();
        save_checkpoint();
        const auto save_ms = seed::milliseconds(save_start, seed::MeasurementClock::now());
        if (benchmark) {
            if (frames != measured_frames + warmup_frames)
                throw std::runtime_error("Benchmark interrupted before all frames were measured");
            const auto generation = world.generation_metrics();
            measurements.write(benchmark,
                               {seed, seed::generator_version, warmup_frames, measured_frames, report_width,
                                report_height, stream_workload, damage_demo, overview, SDL_GetPlatform(),
                                gpu.renderer(), gpu.version(), generation.chunks, generation.nanoseconds,
                                generation.maximum_nanoseconds, save_ms, save},
                               gpu.samples, gpu.skipped);
        }
        if (damage_demo && physics.grounded_unsupported() == 0)
            throw std::runtime_error("Collapse check failed: no unsupported pieces reached the ground");
        std::printf(
            "Presented %u frames; %u draw calls; %u unsupported pieces (%u grounded); changes saved.\n",
            frames, renderer.draw_calls(), physics.unsupported(), physics.grounded_unsupported());
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Engine error: %s\n", error.what());
        return 1;
    }
}
