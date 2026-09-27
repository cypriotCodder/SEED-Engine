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
            check(tile.material == s.material && tile.elevation == s.elevation, "Chunk fill matches samples");
            check(((tile.flags & seed::tile_solid) != 0) == s.solid, "Solid flags follow the rules");
            trees += tile.material == registry.find("tree");
            grass += tile.material == registry.find("grass");
        }
    // Trees replace one grass tile in four, give or take.
    check(trees > 0 && grass > 0 && trees * 10 > (trees + grass) && trees * 10 < (trees + grass) * 4,
          "Scatter density is about one in four");
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
} // namespace

int main() {
    try {
        json_round_trip();
        problems();
        sampling();
        versions();
        std::cout << "Terrain checks passed.\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
