#pragma once
#include "io/json.hpp"
#include "project/assets.hpp"
#include "world/coordinates.hpp"
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace seed {
// A scene as the editor saves it: named entities placed in the world. Components are optional;
// an entity with neither is an empty marker (a spawn point, say) that scripts can find by name.
struct SceneVisual {
    std::string material; // By name, so reordering materials keeps the reference.
    Vec2 size{1, 1};
    bool operator==(const SceneVisual&) const = default;
};
struct SceneLight {
    std::array<float, 3> color{1, 0.85F, 0.6F};
    float radius{6}, intensity{2}, height{2};
    bool operator==(const SceneLight&) const = default;
};
// The player-controlled character's settings: which actions move it, and how the camera follows it.
struct ScenePlayer {
    std::string up{"move_up"}, down{"move_down"}, left{"move_left"}, right{"move_right"}, run{"run"};
    bool input{true};  // Moves with the keys; off leaves movement to its script.
    float zoom{40};    // Camera: logical pixels per tile.
    float smoothing{}; // Camera: seconds to catch up (0 follows exactly).
    float dead_zone{}; // Camera: tiles the player may move before the camera does.
    bool resume{true}; // A saved game starts where the player was, not at this position.
    bool operator==(const ScenePlayer&) const = default;
};
// A character: something that walks with collisions, such as the player or an NPC. NPCs are moved
// by their scripts (walk, walk_to, stop) under the same rules as the player.
struct SceneCharacter {
    float speed{5}, run_speed{8}; // Tiles per second.
    float acceleration{};         // Tiles per second squared to reach full speed; 0 is instant.
    Vec2 collision{0.6F, 0.6F};   // Collision box size.
    bool water{true}, solid{true}, buildings{true}; // What blocks it.
    bool face_movement{};                           // Turns to face the way it walks.
    std::optional<ScenePlayer> player;              // Present on the one player-controlled character.
    bool operator==(const SceneCharacter&) const = default;
};
struct SceneEntity {
    std::string name;
    WorldPosition position{}; // Canonical: local offsets in [0, chunk_side).
    float angle{};            // Radians, counter-clockwise.
    std::optional<SceneVisual> visual;
    std::optional<SceneLight> light;
    std::optional<SceneCharacter> character;
    std::string script; // A file in the project's scripts/ folder, such as "player.lua"; empty for none.
    // The prefab this entity was placed from (prefabs/<name>.json), or empty. A placed prefab keeps
    // its own name, position and angle; its visual, light and script are the prefab's, copied here
    // so games load scenes without resolving prefabs.
    std::string prefab;
    // Editor-only: hidden entities are not drawn in the Scene view; locked ones cannot be picked
    // there. Games ignore both.
    bool hidden{}, locked{};
    bool operator==(const SceneEntity&) const = default;
};
// Whether `name` can name a prefab: 1 to 64 letters, digits, '_' and '-'.
bool valid_prefab_name(std::string_view name);
// A prefab file (prefabs/<name>.json): {"format": 1, "entity": {...}} holding one entity, placed
// at the origin.
Json prefab_json(const SceneEntity& entity);
SceneEntity parse_prefab(const Json& json);
// Gives `placed` the prefab's visual, light and script, keeping its own name and placement.
void apply_prefab(const SceneEntity& prefab, SceneEntity& placed);

// Whether `name` is a plain script file name: letters, digits, '_', '-' and '.', ending in ".lua".
bool valid_script_name(std::string_view name);

struct SceneFile {
    static constexpr std::size_t capacity = 4096; // Entities per scene.
    std::string terrain{"main"};                  // The project terrain this scene's world is generated from.
    std::vector<SceneEntity> entities;
    TerrainPaint paint; // Tiles painted over the terrain, saved beside the scene as <name>.paint.
    bool operator==(const SceneFile&) const = default;

    // Every problem found, one per line, checking material names against `assets`; empty when
    // the scene is valid.
    std::string problems(const Assets& assets) const;
};

Json scene_json(const SceneFile& scene);
Json entity_json(const SceneEntity& entity);
SceneEntity parse_entity(const Json& json);
SceneFile parse_scene(const Json& json);
// Files: scenes/<name>.json, and scenes/<name>.paint when the scene paints its terrain.
std::filesystem::path paint_file(const std::filesystem::path& scene_file);
SceneFile load_scene(const std::filesystem::path& file);
void save_scene(const std::filesystem::path& file, const SceneFile& scene);
} // namespace seed
