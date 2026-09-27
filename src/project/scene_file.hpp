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
struct SceneEntity {
    std::string name;
    WorldPosition position{}; // Canonical: local offsets in [0, chunk_side).
    float angle{};            // Radians, counter-clockwise.
    std::optional<SceneVisual> visual;
    std::optional<SceneLight> light;
    std::string script; // A file in the project's scripts/ folder, such as "player.lua"; empty for none.
    bool operator==(const SceneEntity&) const = default;
};
// Whether `name` is a plain script file name: letters, digits, '_', '-' and '.', ending in ".lua".
bool valid_script_name(std::string_view name);

struct SceneFile {
    static constexpr std::size_t capacity = 4096; // Entities per scene.
    std::vector<SceneEntity> entities;
    bool operator==(const SceneFile&) const = default;

    // Every problem found, one per line, checking material names against `assets`; empty when
    // the scene is valid.
    std::string problems(const Assets& assets) const;
};

Json scene_json(const SceneFile& scene);
SceneFile parse_scene(const Json& json);
SceneFile load_scene(const std::filesystem::path& file);
void save_scene(const std::filesystem::path& file, const SceneFile& scene);
} // namespace seed
