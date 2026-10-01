#include "project/paint.hpp"
#include "io/binary.hpp"
#include "world/coordinates.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace seed {
namespace {
constexpr std::uint32_t paint_magic = 0x544e5053, paint_version = 1; // "SPNT"

std::pair<TerrainPaint::Key, std::uint16_t> locate(std::int64_t x, std::int64_t y) {
    // Arithmetic shifts and masks floor negative coordinates into their chunk.
    return {{x >> 5, y >> 5},
            static_cast<std::uint16_t>((y & (chunk_side - 1)) * chunk_side + (x & (chunk_side - 1)))};
}
static_assert(chunk_side == 32, "locate() assumes 32-tile chunks");

bool by_index(const std::pair<std::uint16_t, PaintedTile>& a, std::uint16_t index) {
    return a.first < index;
}
} // namespace

std::size_t TerrainPaint::tiles() const {
    std::size_t count = 0;
    for (const auto& [key, list] : chunks)
        count += list.size();
    return count;
}

const PaintedTile* TerrainPaint::find(std::int64_t x, std::int64_t y) const {
    const auto [key, index] = locate(x, y);
    const auto chunk = chunks.find(key);
    if (chunk == chunks.end()) return nullptr;
    const auto at = std::lower_bound(chunk->second.begin(), chunk->second.end(), index, by_index);
    return at != chunk->second.end() && at->first == index ? &at->second : nullptr;
}

void TerrainPaint::set(std::int64_t x, std::int64_t y, const PaintedTile& tile) {
    const auto [key, index] = locate(x, y);
    auto& list = chunks[key];
    const auto at = std::lower_bound(list.begin(), list.end(), index, by_index);
    const bool present = at != list.end() && at->first == index;
    if (!tile.mask) {
        if (present) list.erase(at);
    } else if (present)
        at->second = tile;
    else
        list.insert(at, {index, tile});
    if (list.empty()) chunks.erase(key);
}

std::uint8_t TerrainPaint::material(const std::string& name) {
    const auto found = std::find(materials.begin(), materials.end(), name);
    if (found != materials.end()) return static_cast<std::uint8_t>(found - materials.begin());
    if (materials.size() == material_capacity) compact();
    if (materials.size() == material_capacity) throw std::length_error("Paint uses too many materials");
    materials.push_back(name);
    return static_cast<std::uint8_t>(materials.size() - 1);
}

void TerrainPaint::compact() {
    std::vector<int> used(materials.size(), -1);
    for (const auto& [key, list] : chunks)
        for (const auto& [index, tile] : list) {
            if (tile.mask & paint_ground) used[tile.ground] = 0;
            if ((tile.mask & paint_object) && tile.object) used[tile.object - 1u] = 0;
        }
    std::vector<std::string> kept;
    for (std::size_t i = 0; i < used.size(); ++i)
        if (used[i] == 0) {
            used[i] = static_cast<int>(kept.size());
            kept.push_back(std::move(materials[i]));
        }
    for (auto& [key, list] : chunks)
        for (auto& [index, tile] : list) {
            if (tile.mask & paint_ground) tile.ground = static_cast<std::uint8_t>(used[tile.ground]);
            if ((tile.mask & paint_object) && tile.object)
                tile.object = static_cast<std::uint8_t>(used[tile.object - 1u] + 1);
        }
    materials = std::move(kept);
}

std::string TerrainPaint::problems(const std::vector<std::string>& known) const {
    std::string out;
    std::vector<bool> used(materials.size());
    for (const auto& [key, list] : chunks)
        for (const auto& [index, tile] : list) {
            if (tile.mask & paint_ground) used[tile.ground] = true;
            if ((tile.mask & paint_object) && tile.object) used[tile.object - 1u] = true;
        }
    for (std::size_t i = 0; i < materials.size(); ++i)
        if (used[i] && std::find(known.begin(), known.end(), materials[i]) == known.end())
            out += "Painted terrain uses material \"" + materials[i] + "\", which does not exist\n";
    if (tiles() > tile_capacity) out += "Painted terrain has more than 1,048,576 tiles\n";
    return out;
}

std::string encode_paint(const TerrainPaint& source) {
    auto paint = source;
    paint.compact();
    Bytes out;
    out.u32(paint_magic);
    out.u32(paint_version);
    out.u8(static_cast<std::uint8_t>(paint.materials.size()));
    for (const auto& name : paint.materials) {
        out.u8(static_cast<std::uint8_t>(name.size()));
        out.data.insert(out.data.end(), name.begin(), name.end());
    }
    out.u32(static_cast<std::uint32_t>(paint.chunks.size()));
    for (const auto& [key, list] : paint.chunks) {
        out.u64(static_cast<std::uint64_t>(key.first));
        out.u64(static_cast<std::uint64_t>(key.second));
        out.u16(static_cast<std::uint16_t>(list.size()));
        for (const auto& [index, tile] : list) {
            out.u16(index);
            out.u8(tile.mask);
            if (tile.mask & paint_ground) out.u8(tile.ground);
            if (tile.mask & paint_object) out.u8(tile.object);
            if (tile.mask & paint_height) out.u32(std::bit_cast<std::uint32_t>(tile.elevation));
            if (tile.mask & paint_solid) out.u8(tile.solid ? 1 : 0);
        }
    }
    return {out.data.begin(), out.data.end()};
}

TerrainPaint decode_paint(std::string_view bytes) {
    Reader in({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
    if (in.u32() != paint_magic) throw std::runtime_error("Not a terrain paint file");
    if (in.u32() != paint_version) throw std::runtime_error("Terrain paint from a newer editor");
    TerrainPaint paint;
    for (unsigned i = 0, count = in.u8(); i < count; ++i) {
        const auto name = in.take(in.u8());
        paint.materials.emplace_back(name.begin(), name.end());
        if (paint.materials.back().empty()) throw std::runtime_error("Empty material name in terrain paint");
    }
    const auto palette = paint.materials.size();
    std::size_t total = 0;
    for (std::uint32_t c = 0, count = in.u32(); c < count; ++c) {
        const TerrainPaint::Key key{std::bit_cast<std::int64_t>(in.u64()),
                                    std::bit_cast<std::int64_t>(in.u64())};
        if (key.first < -(INT64_MAX >> 5) || key.first > (INT64_MAX >> 5) || key.second < -(INT64_MAX >> 5) ||
            key.second > (INT64_MAX >> 5) || paint.chunks.count(key) ||
            (!paint.chunks.empty() && !(paint.chunks.rbegin()->first < key)))
            throw std::runtime_error("Terrain paint chunks out of order or range");
        auto& list = paint.chunks[key];
        const unsigned tiles = in.u16();
        if (!tiles || tiles > chunk_side * chunk_side) throw std::runtime_error("Invalid painted tile count");
        if ((total += tiles) > TerrainPaint::tile_capacity)
            throw std::runtime_error("Too many painted tiles");
        for (unsigned t = 0; t < tiles; ++t) {
            PaintedTile tile;
            const auto index = in.u16();
            tile.mask = in.u8();
            if (index >= chunk_side * chunk_side || (!list.empty() && list.back().first >= index) ||
                !tile.mask || (tile.mask & ~paint_all))
                throw std::runtime_error("Invalid painted tile");
            if (tile.mask & paint_ground) tile.ground = in.u8();
            if (tile.mask & paint_object) tile.object = in.u8();
            if (tile.mask & paint_height) tile.elevation = std::bit_cast<float>(in.u32());
            if (tile.mask & paint_solid) {
                const auto solid = in.u8();
                if (solid > 1) throw std::runtime_error("Invalid painted tile");
                tile.solid = solid == 1;
            }
            if (((tile.mask & paint_ground) && tile.ground >= palette) ||
                ((tile.mask & paint_object) && tile.object > palette) ||
                ((tile.mask & paint_height) && !(std::abs(tile.elevation) <= 16)))
                throw std::runtime_error("Painted tile out of range");
            list.emplace_back(index, tile);
        }
    }
    if (!in.done()) throw std::runtime_error("Trailing terrain paint data");
    return paint;
}
} // namespace seed
