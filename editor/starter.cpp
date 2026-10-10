#include "starter.hpp"
#include <algorithm>

namespace seed::editor {
namespace {
MaterialAsset material(const char* name, int r, int g, int b, Pattern pattern = Pattern::speckle,
                       int variation = 23) {
    MaterialAsset m;
    m.name = name;
    m.color = {r, g, b};
    m.pattern = pattern;
    m.variation = variation;
    return m;
}
TerrainTerm term(TerrainTerm::Kind kind, unsigned wavelength, float amplitude, bool warped = false) {
    return {kind, wavelength, amplitude, warped};
}
TerrainCondition below(const char* field, float max) {
    TerrainCondition c;
    c.field = field;
    c.max = max;
    return c;
}
TerrainCondition above(const char* field, float min) {
    TerrainCondition c;
    c.field = field;
    c.min = min;
    return c;
}
TerrainRule rule(const char* name, const char* material, std::vector<TerrainCondition> when,
                 const char* tree = nullptr, std::uint32_t one_in = 0) {
    TerrainRule r;
    r.name = name;
    r.material = material;
    r.when = std::move(when);
    if (tree) r.scatter = TerrainScatter{tree, one_in, true};
    return r;
}

} // namespace

void starter_island(Assets& assets, const std::string& terrain) {
    MaterialAsset wanted[] = {
        material("deep_water", 22, 58, 98, Pattern::water, 10),
        material("water", 34, 92, 132, Pattern::water, 12),
        material("sand", 214, 196, 140, Pattern::speckle, 14),
        material("grass", 88, 142, 70),
        material("forest_floor", 58, 96, 50),
        material("mud", 96, 80, 54, Pattern::speckle, 18),
        material("dry_grass", 164, 152, 84),
        material("stone", 122, 122, 128, Pattern::speckle, 30),
        material("snow", 236, 240, 246, Pattern::speckle, 8),
        material("tree", 34, 78, 40, Pattern::round, 30),
    };
    // Ground blends upwards from the sea: each band fades over the one below it.
    const std::pair<const char*, int> blends[] = {{"water", 10}, {"sand", 20},      {"mud", 25},
                                                  {"grass", 30}, {"dry_grass", 30}, {"forest_floor", 40},
                                                  {"stone", 50}, {"snow", 60}};
    for (auto& m : wanted)
        for (const auto& [name, blend] : blends)
            if (m.name == name) m.blend = blend;
    for (const auto& m : wanted)
        if (std::none_of(assets.materials.begin(), assets.materials.end(),
                         [&](const MaterialAsset& e) { return e.name == m.name; }) &&
            assets.materials.size() < Materials::capacity)
            assets.materials.push_back(m);
    using K = TerrainTerm::Kind;
    TerrainAsset& t = assets.terrains[terrain];
    t = {};
    t.island = true;
    t.radius = 64;
    t.coast = 0.12F;
    t.warp = 24;
    t.warp_wavelength = 256;
    t.fields = {
        {"elevation",
         0.34F,
         {term(K::spot, 40, 0.7F), term(K::perlin, 512, 0.75F, true), term(K::fractal, 128, 0.6F, true),
          term(K::ridged, 512, 0.25F, true)}},
        {"moisture", 0, {term(K::fractal, 128, 1, true), term(K::perlin, 512, 0.5F)}},
        {"temperature", 1, {term(K::distance, 1, -1.4F), term(K::perlin, 512, 0.15F)}},
        {"ring", 0, {term(K::distance, 1, 1), term(K::perlin, 1024, 0.12F), term(K::perlin, 256, 0.04F)}},
        // Ridged noise is near 1 along thin winding lines: rivers, where the rule below makes them.
        {"river", 0, {term(K::ridged, 256, 1, true)}},
    };
    t.rules = {
        rule("Deep sea", "deep_water", {below("elevation", -0.25F)}),
        rule("Sea", "water", {below("elevation", 0)}),
        rule("Beach", "sand", {below("elevation", 0.06F)}),
        // Rivers wind across the lowlands, but not through the middle, where the game starts.
        rule("River", "water", {above("river", 0.94F), below("elevation", 0.55F), above("ring", 0.12F)}),
        rule("Snow", "snow", {below("temperature", -0.15F)}),
        rule("Meadow", "grass", {below("ring", 0.28F)}, "tree", 19),
        rule("Swamp", "mud", {below("ring", 0.55F), above("moisture", 0.3F), below("elevation", 0.45F)},
             "tree", 12),
        rule("Forest", "forest_floor", {below("ring", 0.55F)}, "tree", 5),
        rule("Mountain", "stone", {above("elevation", 0.55F)}),
        rule("Plains", "dry_grass", {}, "tree", 60),
    };
    // Ruined stone pillars, now and then, on low dry land.
    TerrainFeature ruins;
    ruins.name = "Ruins";
    ruins.one_in = 10;
    ruins.when = {above("elevation", 0.1F), below("elevation", 0.5F)};
    ruins.rows = {"#.#.#", ".....", "#...#", ".....", "#.#.#"};
    ruins.cells = {{'#', "stone", "", true}};
    t.features = {ruins};
}

} // namespace seed::editor
