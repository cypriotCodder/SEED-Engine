#pragma once

namespace seed {
// Atlas slots. Terrain stores these values per tile, so world generation and the renderer share
// one list. New materials are appended; the order only matters within a single run.
enum class Material : unsigned {
    water,
    sand,
    grass,
    stone,
    wood,
    leaves,
    player,
    ember,
    deep_water,
    forest_floor,
    mud,
    dry_grass,
    snow,
    count
};
} // namespace seed
