#pragma once
#include "core/ecs.hpp"
#include "project/archive.hpp"
#include "world/coordinates.hpp"
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct lua_State;

namespace seed {
class Engine;
class Renderer;
struct DayClock;
struct Tile;

// The tile rule scripts and the project runner move by: water (elevation below 0), solid tiles
// and unloaded ground block.
bool blocks_walking(void*, const Tile* tile);

// Runs a project's Lua gameplay scripts. Each attached script gets its own environment, so its
// globals belong to that entity alone; `self` in it is the entity. A script may define start(),
// called once before its first update, and update(dt), called every fixed step. Scripts see the
// engine through a small API (input, world, ui, sound, particles, camera, game, atmosphere; see
// docs/scripting.md) and nothing else: no files, OS or modules. A failing script is reported and
// switched off; the game carries on.
class ScriptHost final {
public:
    static constexpr std::size_t memory_limit = 64 * 1024 * 1024;
    static constexpr double call_budget_seconds = 0.25;

    // `scripts` reads the project's scripts/ folder, by file name.
    ScriptHost(Engine& engine, ProjectFiles scripts);
    ~ScriptHost();
    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    // Names an entity so world.find can reach it.
    void name(Entity entity, const std::string& name);
    // Loads scripts/<file> for `entity` and runs its top level now.
    void attach(Entity entity, const std::string& file);
    // How world.spawn{prefab = ...} makes an entity: the runner instantiates prefabs, with their
    // lights and scripts. Without one, spawning a prefab is an error.
    using PrefabSpawner = std::function<Entity(const std::string& name, WorldPosition at, float angle)>;
    void set_prefab_spawner(PrefabSpawner spawner) { spawn_prefab_ = std::move(spawner); }
    // The entity game.player() returns.
    void set_player(Entity player) { player_ = player; }
    // The time of day the atmosphere API reads and sets; without one, those calls are errors.
    void set_clock(DayClock* clock) { clock_ = clock; }
    // Runs pending start() calls, then update(dt) on every live script.
    void update(float dt);
    // Calls on_surface(material) on each scripted character that stepped onto a different ground
    // material in the last move_characters (or first stood on one). Call right after it.
    void surfaces();
    // Calls on_touch(other) and on_leave(other) on scripted entities whose box (a character's
    // collision box, else its visual's size, unrotated) starts or stops overlapping another
    // entity's. Call after everything has moved in a step.
    void touches();
    // Where the mouse is, for input.pointer(): in the world, and in logical pixels on a screen of
    // `width` x `height`. Call once a frame.
    void set_pointer(WorldPosition world, float x, float y, int width, int height);
    // Draws what scripts asked the ui module for in the last step, over everything else.
    void draw_ui(Renderer& renderer) const;
    // Whether a script paused the game (game.set_paused): scripts still update, the world waits.
    bool paused() const { return paused_; }
    // The scene game.scene() names; set before its scripts attach.
    void set_scene(const std::string& name) { scene_ = name; }
    // A scene a script asked for with game.load_scene, and the spawn entity's name, once.
    std::optional<std::pair<std::string, std::string>> take_scene_request() {
        return std::exchange(scene_request_, {});
    }
    // Reports a script error that happened outside a script, such as a scene that cannot load.
    void report(const std::string& message);
    unsigned errors() const { return errors_; }

private:
    struct Instance {
        Entity entity;
        int environment; // Registry reference to the script's own global table.
        std::string file;
        bool started{}, failed{};
        std::vector<Entity> touching; // What its box overlapped after the last step.
    };
    struct UiCommand {
        float x, y, width, height, scale;
        float color[4];
        std::string text; // Empty for a rectangle.
    };
    friend struct ScriptApi;
    // Calls `function` from an instance's environment, if the script defines it.
    void call(Instance& instance, const char* function, int arguments);
    void fail(Instance& instance, const std::string& message);
    void forget(Entity entity);

    Engine& engine_;
    ProjectFiles scripts_;
    PrefabSpawner spawn_prefab_;
    Entity player_{};
    DayClock* clock_{};
    std::vector<UiCommand> ui_;
    WorldPosition pointer_{};
    float pointer_x_{}, pointer_y_{};
    int screen_width_{1280}, screen_height_{720};
    bool paused_{};
    std::string scene_;
    std::optional<std::pair<std::string, std::string>> scene_request_;
    lua_State* lua_{};
    std::size_t memory_{};
    std::vector<Instance> instances_;
    std::vector<std::string> names_; // By entity index.
    std::chrono::steady_clock::time_point call_start_{};
    double time_{};
    unsigned errors_{};
};
} // namespace seed
