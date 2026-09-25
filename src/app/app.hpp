#pragma once
#include "core/jobs.hpp"
#include "core/metrics.hpp"
#include "core/particles.hpp"
#include "core/scene.hpp"
#include "io/checkpoint.hpp"
#include "physics/physics.hpp"
#include "platform/audio.hpp"
#include "platform/window.hpp"
#include "render/gpu_timer.hpp"
#include "render/renderer.hpp"
#include "world/world.hpp"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

namespace seed {
// Options every game accepts. The engine parses these; anything else goes to Game::option.
struct AppOptions {
    std::uint64_t seed{};
    std::filesystem::path save;
    bool explicit_save{};
    bool smoke{};                  // 60 fixed frames, no audio or vsync, then exit.
    const char* screenshot{};      // Written on the last smoke frame or on F12.
    const char* benchmark{};       // JSON report path; implies a fixed, input-free run.
    unsigned measured_frames{600}; // Benchmark frames after warmup.
    bool stream_workload{};        // Benchmark route that crosses chunk unload boundaries.
    static constexpr unsigned warmup_frames = 60;
};

// What the camera sees this frame, after the fixed steps.
struct View {
    WorldPosition camera;  // Interpolated focus position at the screen centre.
    WorldPosition pointer; // Mouse position in the world.
    float zoom{};          // Drawable pixels per world unit.
    float alpha{};         // Interpolation factor between the previous and current fixed step.
    int width{}, height{}, logical_width{}, logical_height{};
};

class Engine;

// A game is a set of callbacks the engine calls in a fixed order each frame. Every callback is
// optional except setup. `context` is passed back unchanged, as in Jobs::Job and ChunkHooks.
//
// Order per frame: frame -> (engine streams around the focus) -> step x N -> zoom -> act ->
// render. Physics is idle during frame and step; the last fixed step's physics keeps running on a
// worker through act and render, so act must call engine.physics.finish_step() before touching
// bodies.
struct Game {
    void* context{};
    const char* name{};       // Executable name, used in the usage message.
    const char* title{};      // Window title prefix; the draw-call count is appended.
    const char* asset_pack{}; // Archive file name beside the executable.
    std::uint64_t default_seed{};
    const char* default_save{};
    const char* usage{};                           // Game options, appended to the usage message.
    bool (*option)(void*, std::string_view arg){}; // Return true if the game consumed arg.
    void (*validate)(void*, const AppOptions&){};  // Reject incompatible option combinations.
    // Create the game's entities at the saved focus position; return the entity to follow.
    Entity (*setup)(void*, Engine&, WorldPosition spawn){};
    void (*loaded)(void*, Engine&){};              // Once, after the first chunks are resident.
    void (*frame)(void*, Engine&, float dt){};     // Start of each frame.
    void (*step)(void*, Engine&, float dt){};      // Each fixed step, before physics runs.
    float (*zoom)(void*, Engine&){};               // Logical pixels per world unit.
    void (*act)(void*, Engine&, const View&){};    // Input that needs the camera, e.g. pointing.
    void (*render)(void*, Engine&, const View&){}; // Draw between renderer begin and finish.
    void (*describe)(void*, BenchmarkMetadata&){}; // Game fields of the benchmark report.
    void (*shutdown)(void*, Engine&){};            // After the final checkpoint.
};

// The running engine. Games reach every subsystem through these members.
class Engine final {
public:
    Engine(const Game& game, const AppOptions& options);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Writes world, building and focus deltas into the working save. Physics is joined first.
    void write_deltas(WorldPosition focus);
    // Draws every entity with a Visual near the camera, interpolated between fixed steps.
    void draw_entities(const View& view);
    WorldPosition focus_position();

    const AppOptions options;
    Jobs jobs;

private:
    std::unique_ptr<PackStream> assets_;

public:
    Window window;
    Renderer renderer;
    GpuTimer gpu;
    Scene scene;
    Checkpoint checkpoint;
    World world;
    Physics physics;
    Particles particles;
    Audio audio;
    Input input;
    Entity focus{};
    unsigned frames{};

private:
    friend int run(const Game&, int, char**);
    void loop(const Game& game);
    BenchmarkReport measurements_;
};

// Parses the command line, runs the game until quit, and returns the process exit code. Errors
// are reported on stderr as "Engine error: ...".
int run(const Game& game, int argc, char** argv);
} // namespace seed
