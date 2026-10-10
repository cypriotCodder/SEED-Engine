#pragma once
#include "assets/sound_file.hpp"
#include "core/jobs.hpp"
#include "core/metrics.hpp"
#include "core/particles.hpp"
#include "core/scene.hpp"
#include "io/checkpoint.hpp"
#include "physics/physics.hpp"
#include "platform/actions.hpp"
#include "platform/audio.hpp"
#include "platform/window.hpp"
#include "project/assets.hpp"
#include "render/gpu_timer.hpp"
#include "render/renderer.hpp"
#include "world/world.hpp"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace seed {
// Options every game accepts. The engine parses these; anything else goes to Game::option.
struct AppOptions {
    std::uint64_t seed{};
    std::filesystem::path save;
    bool explicit_save{};
    bool smoke{};             // 60 fixed frames, no audio or vsync, then exit.
    const char* screenshot{}; // Written on the last smoke frame or on F12.
    // With smoke: a text file of timed actions that plays the game, one per line, `<frame>
    // <action> down|up`, frames counted from 0 and never going back, and finally `<frame> end`,
    // the frame the run quits on instead of the 60th (at most 36,000). `#` starts a comment.
    const char* replay{};
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
    const char* id{};         // Stable game identifier recorded in saves; never change it once saves exist.
    const char* title{};      // Window title prefix; the draw-call count is appended.
    const char* asset_pack{}; // Texture pack beside the executable, or an absolute path; optional.
    // A project's assets/ folder (materials, actions, sounds, particle styles made in the editor),
    // relative to the executable unless absolute. Registered before the game's own callbacks run,
    // so those callbacks can add more. Optional.
    const char* project_assets{};
    // Already-loaded project assets, used instead of project_assets; must outlive the Engine.
    const Assets* assets{};
    // Reads the project's assets/ folder by relative path (sounds/, music/), for recorded sounds
    // and music. Defaults to the project_assets folder when that is given.
    ProjectFiles asset_files;
    std::uint64_t default_seed{};
    int window_width{1280}, window_height{720}; // Logical pixels.
    bool fullscreen{};                          // Ignored by automated (--smoke) runs.
    const char* default_save{};
    const char* usage{}; // Game options, appended to the usage message.
    // How the game's world is generated and edited. Leave `terrain` unset to use the project's
    // terrain.json (see project_assets) instead.
    WorldGenerator world{};
    // Register every material, in a fixed order, before the renderer or world generation start.
    void (*materials)(void*, Materials&){};
    // Register the game's input actions with their default bindings. Optional.
    void (*actions)(void*, Actions&){};
    // Register the game's sounds and particle styles. Optional.
    void (*effects)(void*, Sounds&, Particles&){};
    // How building bodies look; see BodyVisuals. Optional.
    Visual (*body_visual)(void*, const BodyState&){};
    float body_lift_per_height{}; // See BodyVisuals::lift_per_height.
    // Starts every save file's name, so one save can hold several worlds; null for none.
    const char* save_prefix{};
    std::size_t scene_memory{};                    // Scene arena bytes; 0 uses Scene::default_memory.
    bool (*option)(void*, std::string_view arg){}; // Return true if the game consumed arg.
    void (*validate)(void*, const AppOptions&){};  // Reject incompatible option combinations.
    // Create the game's entities at the saved focus position; return the entity to follow.
    Entity (*setup)(void*, Engine&, WorldPosition spawn){};
    void (*loaded)(void*, Engine&){};              // Once, after the first chunks are resident.
    void (*frame)(void*, Engine&, float dt){};     // Start of each frame.
    void (*step)(void*, Engine&, float dt){};      // Each fixed step, before physics runs.
    float (*zoom)(void*, Engine&){};               // Logical pixels per world unit; else camera_zoom.
    void (*act)(void*, Engine&, const View&){};    // Input that needs the camera, e.g. pointing.
    void (*render)(void*, Engine&, const View&){}; // Draw between renderer begin and finish.
    void (*describe)(void*, BenchmarkMetadata&){}; // Game fields of the benchmark report.
    // Saved entities (see Engine::create_saved) keep their Transform and Visual automatically.
    // These write and read the game's other components for one entity, at most
    // entity_payload_capacity bytes; load must read exactly what save wrote. Optional, as a pair.
    void (*save_entity)(void*, Engine&, Entity, Bytes&){};
    void (*load_entity)(void*, Engine&, Entity, Reader&){};
    // The game's own state beyond its worlds (scores, progress, the time of day), kept in the
    // save's game.state, which belongs to no scene. save writes it, at most game_state_capacity
    // bytes, whenever the worlds are saved; load reads it once, before setup, only when the save
    // has one, and must read exactly what save wrote. Optional, as a pair.
    void (*save_state)(void*, Engine&, Bytes&){};
    void (*load_state)(void*, Engine&, Reader&){};
    void (*shutdown)(void*, Engine&){}; // After the final checkpoint.
};
inline constexpr std::size_t game_state_capacity = 4 * 1024 * 1024;

// The running engine. Games reach every subsystem through these members.
class Engine final {
public:
    Engine(const Game& game, const AppOptions& options);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Writes world, building and focus deltas, and the game's state, into the working save.
    // Physics is joined first.
    void write_deltas(WorldPosition focus);
    // Draws every entity with a Visual near the camera, interpolated between fixed steps. With
    // sort_by_y, it draws them together with the sprites queued since the last call, from the back
    // (the highest bottom edge) to the front, so things further down the screen stand in front;
    // otherwise in entity order, over the queued sprites (drawn back to front among themselves).
    void draw_entities(const View& view);
    // A sprite drawn with the entities: a tile object, say. Coordinates are camera-relative.
    struct QueuedSprite {
        MaterialId material{};
        float x{}, y{}, width{1}, height{1}, angle{};
        unsigned frame{Renderer::automatic};
    };
    void queue_sprite(const QueuedSprite& sprite);
    bool sort_by_y{};
    WorldPosition focus_position();
    // Plays assets/music/<name>.ogg, replacing any music playing. Throws if it is missing or not Ogg
    // Vorbis. With audio off, music is checked and counts as playing, but is not decoded.
    void play_music(const std::string& name, bool loop);
    void stop_music();
    bool music_playing() const { return music_ != nullptr; }
    // Saves where the focus is, then continues in another world from the project's terrain: one
    // terrain and its paint, a seed, and the save-file prefix that keeps its files apart (see
    // World). Call from Game::step. Entities are the game's to remove and create.
    void change_world(const TerrainAsset& terrain, const TerrainPaint& paint, std::uint64_t seed,
                      std::string prefix);
    // Creates an entity that is saved with the world, in the chunk it stands in. It leaves the
    // scene when that chunk unloads and returns when it loads again, as a new Entity handle.
    Entity create_saved(Transform transform, Visual visual);

    const AppOptions options;
    Jobs jobs;

private:
    std::vector<QueuedSprite> queued_; // Cleared each frame; keeps its capacity.
    std::unique_ptr<PackStream> assets_;
    Assets project_assets_; // Registries point at its names, so it is declared before them.
    ProjectFiles asset_files_;
    std::vector<std::vector<float>> sound_samples_; // Recorded sounds; the registry points into them.
    std::unique_ptr<MusicStream> music_;
    void pump_music(); // Keeps the audio's music ring filled; once a frame.

public:
    Materials materials;

private:
    std::unique_ptr<Terrain> terrain_; // The project's terrain, when the game has no terrain code.

public:
    Window window;
    Renderer renderer;
    GpuTimer gpu;
    Scene scene;
    Checkpoint checkpoint;
    World world;
    Physics physics;
    Particles particles;
    Sounds sounds;
    Audio audio;
    Input input;     // Raw input; prefer actions.
    Actions actions; // Engine actions (see engine_action), then the game's.
    Entity focus{};
    // How the camera follows the focus: seconds to catch up (0 follows exactly) and a dead zone in
    // tiles the focus may move before the camera does. Games may change both at any time.
    float camera_smoothing{}, camera_dead_zone{};
    float camera_zoom{40}; // Logical pixels per tile, when the game has no zoom callback.
    unsigned frames{};
    double time{}; // Seconds of game time, counted in fixed steps; animated materials play by it.

private:
    friend int run(const Game&, int, char**);
    void loop(const Game& game);
    WorldPosition follow(WorldPosition target, float dt);
    WorldPosition camera_{};
    bool camera_placed_{};
    void update_owners();
    void restore(ChunkCoord coord, Chunk& chunk);
    bool capture(ChunkCoord coord, Chunk& chunk, bool remove);
    BenchmarkReport measurements_;
    ChunkHooks physics_hooks_;
    void* game_context_;
    void (*save_entity_)(void*, Engine&, Entity, Bytes&);
    void (*load_entity_)(void*, Engine&, Entity, Reader&);
    void (*save_state_)(void*, Engine&, Bytes&);
    std::vector<Entity> captured_;
    struct ReplayEvent {
        unsigned frame;
        ActionId action;
        bool down;
    };
    std::vector<ReplayEvent> replay_;
    std::size_t replay_next_{};
    unsigned last_frame_{60}; // Where a smoke run ends.
    void read_replay(const char* path);
};

// Parses the command line, runs the game until quit, and returns the process exit code. Errors
// are reported on stderr as "Engine error: ...".
int run(const Game& game, int argc, char** argv);
} // namespace seed
