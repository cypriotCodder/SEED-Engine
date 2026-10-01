#include "project/terrain.hpp"
#include <iostream>
#include <memory>

namespace {
using seed::TerrainTerm;
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

seed::Materials materials() {
    seed::Materials registry;
    for (const char* name : {"water", "sand", "grass", "tree", "stone"})
        registry.add({name, {100, 100, 100}});
    return registry;
}
std::vector<std::string> names() {
    return {"water", "sand", "grass", "tree", "stone"};
}

// Land rising towards the centre, noisy enough to have coasts, with trees on grass.
seed::TerrainAsset island() {
    seed::TerrainAsset t;
    t.radius = 8;
    t.warp = 8;
    t.fields = {{"elevation",
                 0.2F,
                 {{TerrainTerm::Kind::spot, 60, 0.5F, false}, {TerrainTerm::Kind::perlin, 64, 0.4F, true}}},
                {"ring", 0, {{TerrainTerm::Kind::distance, 1, 1, false}}}};
    seed::TerrainRule water{"Sea", "water", {{"elevation", -1e30F, 0}}, false, std::nullopt};
    water.when[0].min = -std::numeric_limits<float>::infinity();
    seed::TerrainRule beach{"Beach", "sand", {{"elevation", 0, 0.05F}}, false, std::nullopt};
    seed::TerrainRule peak{
        "Peak", "stone", {{"elevation", 0.6F, std::numeric_limits<float>::infinity()}}, true, std::nullopt};
    seed::TerrainRule grass{"Grass", "grass", {}, false, seed::TerrainScatter{"tree", 4, true}};
    t.rules = {water, beach, peak, grass};
    return t;
}

void json_round_trip() {
    const auto t = island();
    const auto text = seed::to_json(seed::terrain_json(t));
    check(seed::parse_terrain(seed::parse_json(text)) == t, "Terrain reads back unchanged");
    check(text.find("inf") == std::string::npos, "Open range ends are left out of the file");
}

void problems() {
    check(island().problems(names()).empty(), "Sample terrain is valid");
    auto t = island();
    t.fields[0].name = "height";                           // No elevation field any more.
    t.fields[1].terms[0].kind = TerrainTerm::Kind::perlin; // Distance's wavelength 1 is fine for
    t.fields[1].terms[0].wavelength = 100;                 // perlin only if a power of two.
    t.rules[0].material = "lava";
    t.rules[1].when[0].min = 1; // Above its max of 0.05.
    const auto report = t.problems(names());
    for (const char* expected : {"\"elevation\" is required", "powers of two", "unknown material \"lava\"",
                                 "unknown field \"elevation\"", "minimum exceeds"})
        check(report.find(expected) != std::string::npos, expected);
    check(seed::TerrainAsset{}.problems(names()).empty() && !seed::TerrainAsset{}.enabled(),
          "An empty terrain is valid and disabled");
}

void sampling() {
    const auto registry = materials();
    const seed::Terrain terrain(island(), registry);
    const auto centre = terrain.sample(7, {{}, {0.5F, 0.5F}});
    check(centre.elevation > 0.5F && centre.material != registry.find("water"), "The centre is land");
    const auto far = terrain.sample(7, {{40, -40}, {3, 3}});
    check(far.elevation == -1 && far.material == registry.find("water"), "Past the rim is open sea");
    check(terrain.sample(7, {{1, 2}, {5.5F, 6.5F}}).material ==
              terrain.sample(7, {{1, 2}, {5.5F, 6.5F}}).material,
          "Sampling is deterministic");

    // A chunk filled by the generator matches sampling tile centres, including across a border.
    const auto generator = terrain.generator();
    auto chunk = std::make_unique<seed::Chunk>();
    seed::fill_chunk(generator, 7, {1, 0}, *chunk);
    unsigned trees = 0, grass = 0;
    for (int y = 0; y < seed::chunk_side; ++y)
        for (int x = 0; x < seed::chunk_side; ++x) {
            const auto& tile = chunk->tiles[static_cast<std::size_t>(y * seed::chunk_side + x)];
            const auto s = terrain.sample(
                7, {{0, 0}, {32.0F + static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F}});
            check(tile.material == s.material && tile.object == s.object && tile.elevation == s.elevation,
                  "Chunk fill matches samples");
            check(((tile.flags & seed::tile_solid) != 0) == s.solid, "Solid flags follow the rules");
            check(tile.material != registry.find("tree"), "Objects never replace the ground");
            if (tile.material == registry.find("grass")) {
                ++grass;
                trees += tile.object == seed::tile_object(registry.find("tree"));
            }
        }
    // A tree stands on one grass tile in four, give or take.
    check(trees > 0 && trees * 10 > grass && trees * 10 < grass * 4, "Scatter density is about one in four");
}

void versions() {
    const auto registry = materials();
    const auto base = seed::Terrain(island(), registry).version();
    auto reseeded = island();
    reseeded.default_seed = 99;
    check(seed::Terrain(reseeded, registry).version() == base,
          "The default seed does not change the version");
    auto changed = island();
    changed.fields[0].terms[1].amplitude = 0.41F;
    check(seed::Terrain(changed, registry).version() != base, "Any setting change changes the version");
    auto broken = island();
    broken.rules[0].material = "lava";
    try {
        seed::Terrain(broken, registry);
        throw std::logic_error("Invalid terrain compiled");
    } catch (const std::invalid_argument&) {}
}
// Painted tiles: stored sparsely by chunk, saved compactly, and applied over the generated
// terrain, changing its version so saves of the unpainted world are not mixed in.
void painting() {
    seed::TerrainPaint paint;
    check(paint.empty() && !paint.find(0, 0), "Nothing painted at first");
    seed::PaintedTile ground{seed::paint_ground, paint.material("stone"), 0, false, 0};
    paint.set(-1, -1, ground); // Chunk (-1, -1), its top-right tile.
    paint.set(33, 2, ground);
    seed::PaintedTile pond{seed::paint_height | seed::paint_solid | seed::paint_object, 0, 0, true, -0.5F};
    paint.set(5, 6, pond);
    seed::PaintedTile tree{seed::paint_object, 0, static_cast<std::uint8_t>(paint.material("tree") + 1),
                           false, 0};
    paint.set(7, 6, tree);
    check(paint.tiles() == 4 && paint.chunks.size() == 3, "Tiles are kept by chunk");
    check(paint.find(-1, -1) && *paint.find(-1, -1) == ground && !paint.find(-2, -1),
          "Negative tiles are found");
    check(paint.chunks.count({-1, -1}) && paint.chunks.at({-1, -1})[0].first == 31 * 32 + 31,
          "Negative coordinates floor into their chunk");

    const auto bytes = seed::encode_paint(paint);
    check(seed::decode_paint(bytes) == paint, "Paint reads back unchanged");
    // Header 8, names 1 + 6 + 5, chunk count 4, three chunk headers of 18, then each tile's index
    // and mask (3) and only its painted fields: 1 + 1 + (1 + 4 + 1) + 1.
    check(bytes.size() == 8 + 12 + 4 + 3 * 18 + 4 * 3 + 9, "Only painted fields are stored");
    for (std::size_t cut = 0; cut < bytes.size(); ++cut)
        try {
            seed::decode_paint(std::string_view(bytes).substr(0, cut));
            throw std::logic_error("Truncated paint read");
        } catch (const std::runtime_error&) {}
    auto unknown = paint;
    unknown.material("lava");
    unknown.set(8, 8, {seed::paint_ground, unknown.material("lava"), 0, false, 0});
    check(unknown.problems(names()).find("\"lava\"") != std::string::npos, "Unknown materials are problems");
    unknown.set(8, 8, {});
    check(unknown.problems(names()).empty() && unknown.tiles() == 4, "Erasing restores the generated tile");
    unknown.compact();
    check(unknown.materials == std::vector<std::string>{"stone", "tree"} && *unknown.find(7, 6) == tree,
          "Compacting drops unused names and renumbers");

    const auto registry = materials();
    const seed::Terrain plain(island(), registry), painted(island(), registry, paint);
    check(painted.version() != plain.version(), "Paint changes the version");
    check(seed::Terrain(island(), registry, seed::decode_paint(bytes)).version() == painted.version(),
          "The same paint gives the same version");
    auto chunk = std::make_unique<seed::Chunk>();
    seed::fill_chunk(painted.generator(), 7, {0, 0}, *chunk);
    const auto& wet = chunk->tiles[6 * 32 + 5];
    check(wet.elevation == -0.5F && (wet.flags & seed::tile_solid) && wet.object == seed::no_object,
          "Height, blocking and object removal apply");
    const auto generated = plain.sample(7, {{}, {7.5F, 6.5F}});
    const auto& planted = chunk->tiles[6 * 32 + 7];
    check(planted.object == seed::tile_object(registry.find("tree")) &&
              planted.material == generated.material && planted.elevation == generated.elevation,
          "A painted object keeps the generated ground");
    seed::fill_chunk(painted.generator(), 7, {1, 0}, *chunk);
    check(chunk->tiles[2 * 32 + 1].material == registry.find("stone"),
          "Ground paint applies in its own chunk");
    auto bad = paint;
    bad.set(9, 9, {seed::paint_ground, bad.material("lava"), 0, false, 0});
    try {
        seed::Terrain(island(), registry, bad);
        throw std::logic_error("Paint with an unknown material compiled");
    } catch (const std::invalid_argument&) {}
}
} // namespace

int main() {
    try {
        json_round_trip();
        problems();
        sampling();
        versions();
        painting();
        std::cout << "Terrain checks passed.\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
