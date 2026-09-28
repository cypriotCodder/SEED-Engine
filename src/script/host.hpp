#pragma once
#include "core/ecs.hpp"
#include "project/archive.hpp"
#include "world/coordinates.hpp"
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

struct lua_State;

namespace seed {
class Engine;
struct Tile;

// The tile rule scripts and the project runner move by: water (elevation below 0), solid tiles
// and unloaded ground block.
bool blocks_walking(void*, const Tile* tile);

// Runs a project's Lua gameplay scripts. Each attached script gets its own environment, so its
// globals belong to that entity alone; `self` in it is the entity. A script may define start(),
// called once before its first update, and update(dt), called every fixed step. Scripts see the
// engine through a small API (input, world, sound, particles, camera, game; see
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
    // Runs pending start() calls, then update(dt) on every live script.
    void update(float dt);
    unsigned errors() const { return errors_; }

private:
    struct Instance {
        Entity entity;
        int environment; // Registry reference to the script's own global table.
        std::string file;
        bool started{}, failed{};
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
    lua_State* lua_{};
    std::size_t memory_{};
    std::vector<Instance> instances_;
    std::vector<std::string> names_; // By entity index.
    std::chrono::steady_clock::time_point call_start_{};
    double time_{};
    unsigned errors_{};
};
} // namespace seed
