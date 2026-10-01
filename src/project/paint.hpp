#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace seed {
// Tiles painted over a scene's generated terrain in the editor, saved beside the scene as
// scenes/<scene>.paint. A painted tile replaces some of what the terrain generates there (the
// fields its mask names); the rest stays generated. Games apply paint while generating chunks,
// so painted tiles cost nothing per frame and saves still hold only differences.
constexpr std::uint8_t paint_ground = 1, paint_object = 2, paint_height = 4, paint_solid = 8;
constexpr std::uint8_t paint_all = paint_ground | paint_object | paint_height | paint_solid;

struct PaintedTile {
    std::uint8_t mask{};   // paint_* bits: which of the fields below apply.
    std::uint8_t ground{}; // Index into TerrainPaint::materials.
    std::uint8_t object{}; // 0 for no object, otherwise an index into TerrainPaint::materials + 1.
    bool solid{};
    float elevation{};
    bool operator==(const PaintedTile&) const = default;
};

struct TerrainPaint {
    static constexpr std::size_t material_capacity = 255, tile_capacity = 1 << 20;
    using Key = std::pair<std::int64_t, std::int64_t>; // Chunk coordinate.
    using Tiles = std::vector<std::pair<std::uint16_t, PaintedTile>>;
    // Material names the tiles use, so reordering or adding materials keeps the paint.
    std::vector<std::string> materials;
    // Each chunk's painted tiles, sorted by tile index (y * chunk_side + x). No chunk is empty.
    std::map<Key, Tiles> chunks;
    bool operator==(const TerrainPaint&) const = default;

    bool empty() const { return chunks.empty(); }
    std::size_t tiles() const;
    // The painted tile at global tile (x, y), or null.
    const PaintedTile* find(std::int64_t x, std::int64_t y) const;
    // Paints the tile at (x, y); a mask of 0 removes it, leaving the generated tile.
    void set(std::int64_t x, std::int64_t y, const PaintedTile& tile);
    // The index of the material `name`, added if the tiles do not use it yet. Throws when full.
    std::uint8_t material(const std::string& name);
    // Drops material names no tile uses, renumbering the tiles. Saving compacts.
    void compact();
    // Every problem, one per line; names are checked against `materials`.
    std::string problems(const std::vector<std::string>& materials) const;
};

// The .paint file: little-endian binary, only the painted tiles and only their painted fields.
std::string encode_paint(const TerrainPaint& paint);
TerrainPaint decode_paint(std::string_view bytes); // Throws on malformed data.
} // namespace seed
