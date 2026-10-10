#pragma once
#include "core/material.hpp"
#include "io/json.hpp"
#include "project/paint.hpp"
#include "world/world_generator.hpp"
#include <array>
#include <cstdint>
#include <limits>
#include <map>
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
// A small pattern of tiles stamped into the world: ruins, a stone circle, a pond. A chunk gets one
// with a chance of 1 in `one_in`, at a place picked from the seed and chunk, where the conditions
// hold at the pattern's centre. The pattern always lies within one chunk, so generation stays a pure
// function of the seed and coordinate.
struct TerrainFeatureCell {
    char symbol{'#'};   // The character standing for this cell in the rows.
    std::string ground; // Material laid; empty keeps the generated ground.
    std::string object; // Object placed; empty removes any object.
    bool solid{};
    bool operator==(const TerrainFeatureCell&) const = default;
};
struct TerrainFeature {
    static constexpr std::size_t side_capacity = 16, cell_capacity = 16;
    std::string name;
    std::uint32_t one_in{8};
    std::vector<TerrainCondition> when;
    // Rows of symbols, the top (north) row first, all the same width; '.' leaves a tile as it is.
    std::vector<std::string> rows{"#"};
    std::vector<TerrainFeatureCell> cells{TerrainFeatureCell{}};
    bool operator==(const TerrainFeature&) const = default;
};

struct TerrainAsset {
    static constexpr std::size_t field_capacity = 8, term_capacity = 16, rule_capacity = 64,
                                 feature_capacity = 16;
    bool island{true};  // A disc of land in endless ocean; otherwise the world never ends.
    float radius{64};   // World radius in chunks; scales `distance` terms either way.
    float coast{0.12F}; // Island coast falloff, as a fraction of the radius.
    float warp{};       // Domain warp distance in tiles; 0 disables.
    unsigned warp_wavelength{256};
    std::uint64_t default_seed{1};
    // How strongly slopes are shaded, as if lit from the north-west: 0 draws the ground flat.
    // Only how the world looks; it does not change what is generated.
    float relief{};
    std::vector<TerrainField> fields;     // One must be named "elevation".
    std::vector<TerrainRule> rules;       // Empty: no terrain; every tile is the first material.
    std::vector<TerrainFeature> features; // Stamped over the rules' tiles, in order; paint goes over them.
    bool operator==(const TerrainAsset&) const = default;

    bool enabled() const { return !rules.empty(); }
    // Every problem, one per line; materials are checked against `materials` by name.
    std::string problems(const std::vector<std::string>& materials) const;
};

// The shade of a land tile at `elevation` from its neighbours' heights, lit from the north-west and
// scaled by TerrainAsset::relief: 1 on level ground and water (elevation below 0), lighter on slopes
// facing the light and darker on those facing away, within 0.7 to 1.25.
float relief_shade(float relief, float elevation, float east, float west, float north, float south);

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
    // Throws if the asset or the paint has problems against `materials`. Painted tiles replace
    // generated ones in the chunks this generates.
    Terrain(const TerrainAsset& asset, const Materials& materials, const TerrainPaint& paint = {});
    // The generated tile at a position, features included, without paint.
    Sample sample(std::uint64_t seed, WorldPosition position) const;
    // Changes whenever the settings or the paint would generate a different world (not with
    // default_seed).
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
    struct Painted {
        std::uint16_t index;
        std::uint8_t mask;
        MaterialId ground;
        std::uint8_t object; // As Tile::object.
        bool solid;
        float elevation;
    };
    struct Feature {
        std::uint64_t stream;
        std::uint32_t one_in;
        std::vector<Condition> when;
        int width, height;
        std::vector<std::int8_t> layout; // Per tile, top row first: an index into `cells`, or -1.
        struct Cell {
            bool ground;
            MaterialId material;
            std::uint8_t object; // As Tile::object.
            bool solid;
        };
        std::vector<Cell> cells;
    };
    // Every field's value at a position, the island's coast applied to elevation.
    std::array<float, TerrainAsset::field_capacity> values(std::uint64_t seed, WorldPosition position) const;
    static void fill(void* context, std::uint64_t seed, ChunkCoord coord, Chunk& chunk);
    std::vector<Feature> features_;
    TerrainAsset asset_;
    std::map<TerrainPaint::Key, std::vector<Painted>> paint_;
    std::vector<Field> fields_;
    std::vector<Rule> rules_;
    std::size_t elevation_{};
    std::size_t materials_{}; // Registered materials, for checking saved tiles.
    std::uint32_t version_{};
};
} // namespace seed
