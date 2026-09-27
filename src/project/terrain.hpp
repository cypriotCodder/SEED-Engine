#pragma once
#include "core/material.hpp"
#include "io/json.hpp"
#include "world/world_generator.hpp"
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace seed {
// A world generator described as data (assets/terrain.json). Named fields are sums of noise
// terms; an ordered rule list then picks each tile's material from the field values. The first
// rule whose conditions all hold wins; a tile no rule matches takes the last rule's material.
struct TerrainTerm {
    enum class Kind : std::uint8_t {
        perlin,   // Smooth noise in [-1, 1] at `wavelength` tiles.
        fractal,  // Several octaves of detail, wavelengths 128 down to 8.
        ridged,   // 1 - 2|perlin|: sharp crests, for mountain ranges.
        distance, // Distance from the world centre in world radii (0 at the centre, 1 at the rim).
        spot,     // exp(-(d / wavelength)^2), d in tiles: a bump at the centre.
    };
    Kind kind{Kind::perlin};
    unsigned wavelength{256}; // Power of two, 1-1024 (perlin, ridged); any 1-1024 for spot.
    float amplitude{1};
    bool warped{}; // Sample at the domain-warped position.
    bool operator==(const TerrainTerm&) const = default;
};
struct TerrainField {
    std::string name;
    float base{};
    std::vector<TerrainTerm> terms;
    bool operator==(const TerrainField&) const = default;
};
struct TerrainCondition {
    std::string field;
    float min{-std::numeric_limits<float>::infinity()}, max{std::numeric_limits<float>::infinity()};
    bool operator==(const TerrainCondition&) const = default;
};
struct TerrainScatter {
    std::string material; // The object placed over the ground (Tile::object).
    std::uint32_t one_in{20};
    bool solid{true};
    bool operator==(const TerrainScatter&) const = default;
};
struct TerrainRule {
    std::string name, material;
    std::vector<TerrainCondition> when;
    bool solid{};
    std::optional<TerrainScatter> scatter;
    bool operator==(const TerrainRule&) const = default;
};
struct TerrainAsset {
    static constexpr std::size_t field_capacity = 8, term_capacity = 16, rule_capacity = 64;
    bool island{true};  // A disc of land in endless ocean; otherwise the world never ends.
    float radius{64};   // World radius in chunks; scales `distance` terms either way.
    float coast{0.12F}; // Island coast falloff, as a fraction of the radius.
    float warp{};       // Domain warp distance in tiles; 0 disables.
    unsigned warp_wavelength{256};
    std::uint64_t default_seed{1};
    std::vector<TerrainField> fields; // One must be named "elevation".
    std::vector<TerrainRule> rules;   // Empty: no terrain; every tile is the first material.
    bool operator==(const TerrainAsset&) const = default;

    bool enabled() const { return !rules.empty(); }
    // Every problem, one per line; materials are checked against `materials` by name.
    std::string problems(const std::vector<std::string>& materials) const;
};

Json terrain_json(const TerrainAsset& terrain);
TerrainAsset parse_terrain(const Json& json);

// A terrain asset compiled for generation: names resolved, ready for many threads to sample.
class Terrain final {
public:
    struct Sample {
        float elevation{};
        MaterialId material{};
        std::uint8_t object{no_object}; // As Tile::object.
        bool solid{};
    };
    // Throws if the asset has problems against `materials`.
    Terrain(const TerrainAsset& asset, const Materials& materials);
    Sample sample(std::uint64_t seed, WorldPosition position) const;
    // Changes whenever the settings would generate a different world (not with default_seed).
    std::uint32_t version() const { return version_; }
    // A generator for World. This Terrain must outlive it.
    WorldGenerator generator() const;

private:
    struct Term {
        TerrainTerm term;
        std::uint64_t stream;
    };
    struct Field {
        float base;
        std::vector<Term> terms;
    };
    struct Condition {
        std::size_t field;
        float min, max;
    };
    struct Rule {
        MaterialId material;
        bool solid;
        std::vector<Condition> when;
        bool scatter;
        MaterialId scatter_material;
        std::uint32_t one_in;
        bool scatter_solid;
    };
    static void fill(void* context, std::uint64_t seed, ChunkCoord coord, Chunk& chunk);
    TerrainAsset asset_;
    std::vector<Field> fields_;
    std::vector<Rule> rules_;
    std::size_t elevation_{};
    std::uint32_t version_{};
};
} // namespace seed
