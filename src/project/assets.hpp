#pragma once
#include "core/material.hpp"
#include "core/particles.hpp"
#include "io/json.hpp"
#include "platform/actions.hpp"
#include "platform/audio.hpp"
#include "project/archive.hpp"
#include "project/terrain.hpp"
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace seed {
// What a project defines as data instead of code: materials, input actions, sounds and particle
// styles. The editor edits these; a game registers them at startup. List order is registration
// order, so it decides each entry's ID: tiles store material IDs, so reordering materials changes
// saved worlds.
struct MaterialAsset {
    std::string name;
    std::array<int, 3> color{128, 128, 128};
    Pattern pattern{Pattern::speckle};
    int variation{23};
    std::string texture;    // A texture from assets/textures, by name; empty uses the generated tile.
    float texture_scale{1}; // Tiles one copy of the texture covers on the ground.
    float speed{1};         // Walking speed on this ground, times the character's own (0.1 to 4).
    std::string tags;       // Words for scripts, separated by spaces, such as "slippery hurts".
    bool operator==(const MaterialAsset&) const = default;
};
struct ActionAsset {
    std::string name;
    std::vector<Binding> bindings;
    bool operator==(const ActionAsset&) const = default;
};
struct SoundAsset {
    std::string name;
    float frequency{140}, variation{}, gain{0.2F}, decay{0.9991F}, tone{0.75F};
    bool operator==(const SoundAsset&) const = default;
    SoundDesc desc() const { return {name.c_str(), frequency, variation, gain, decay, tone}; }
};
struct ParticleAsset {
    std::string name;
    std::string material; // By name, so reordering materials keeps the reference.
    unsigned count{12};
    float speed{1}, speed_range{2}, life{0.7F}, size{0.13F}, drag{0.94F}, spin{4}, shade{1.5F};
    bool operator==(const ParticleAsset&) const = default;
};

struct Assets {
    std::vector<MaterialAsset> materials;
    std::vector<ActionAsset> actions;
    std::vector<SoundAsset> sounds;
    std::vector<ParticleAsset> particles;
    // Every terrain of the project by name (assets/terrains/<name>.json); each scene picks one.
    std::map<std::string, TerrainAsset> terrains;
    // The terrain a running game generates its world from: its start scene's choice, and the
    // tiles that scene paints over it.
    TerrainAsset terrain;
    TerrainPaint paint;
    bool operator==(const Assets&) const = default;
    std::vector<std::string> material_names() const;

    // Registers every asset. The registries keep pointers to these names, so this Assets must
    // outlive them and must not change while they are in use. Throws on the first invalid entry,
    // with the registry's own message.
    void register_materials(Materials& out) const;
    void register_actions(Actions& out) const; // After add_engine_actions.
    void register_effects(const Materials& materials, Sounds& sounds, Particles& particles) const;
    // Registers everything into scratch registries and returns every problem found, one per
    // line; empty when the assets are valid. Uses the same checks as a running game.
    std::string problems() const;
};

// Binding text used in project files and shown in the editor: a key's SDL name ("W", "Left
// Shift") or "Mouse Left", "Mouse Middle", "Mouse Right", "Mouse X1", "Mouse X2".
std::string binding_name(const Binding& binding);
Binding parse_binding(std::string_view name); // Throws for an unknown name.

// Project files live in the project's assets/ folder, one per kind: materials.json,
// actions.json, sounds.json and particles.json, plus terrains/<name>.json. A missing file is an
// empty list. A folder's load fills `terrains`, reading an older project's single terrain.json as
// the terrain "main".
Assets load_assets(const std::filesystem::path& folder);
// The same, reading "<kind>.json" through `files` (a folder or an exported game's archive);
// terrains are loaded one at a time with load_terrain, as a game needs only one.
Assets load_assets(const ProjectFiles& files);
// One terrain by name from an assets folder reader; empty (no terrain) when it does not exist.
TerrainAsset load_terrain(const ProjectFiles& assets, const std::string& name);
bool valid_terrain_name(std::string_view name); // Letters, digits, '_' and '-', 1 to 64.
Json terrain_file_json(const TerrainAsset& terrain);
TerrainAsset parse_terrain_file(const Json& file);
// Writes the files whose contents changed since `previous` (all of them if null), atomically.
void save_assets(const std::filesystem::path& folder, const Assets& assets, const Assets* previous = nullptr);
Json assets_json(const Assets& assets, std::string_view kind);
} // namespace seed
