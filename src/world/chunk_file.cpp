#include "world/chunk_file.hpp"
#include "io/binary.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <string>

namespace seed {
namespace {
void put_float(Bytes& out, float value) {
    out.u32(std::bit_cast<std::uint32_t>(value));
}
float get_float(Reader& in) {
    const float value = std::bit_cast<float>(in.u32());
    if (!std::isfinite(value)) throw std::runtime_error("Non-finite body record");
    return value;
}
void put_position(Bytes& out, WorldPosition value) {
    out.u64(std::uint64_t(value.chunk.x));
    out.u64(std::uint64_t(value.chunk.y));
    put_float(out, value.local.x);
    put_float(out, value.local.y);
}
WorldPosition get_position(Reader& in) {
    WorldPosition result;
    result.chunk.x = std::bit_cast<std::int64_t>(in.u64());
    result.chunk.y = std::bit_cast<std::int64_t>(in.u64());
    result.local.x = get_float(in);
    result.local.y = get_float(in);
    const bool canonical = result.local.x >= 0 && result.local.y >= 0 && result.local.x < chunk_side &&
                           result.local.y < chunk_side;
    if (!canonical) throw std::runtime_error("Non-canonical body position");
    return result;
}
BodyState decode_body(Reader& in, ChunkCoord owner) {
    BodyState body;
    const auto exists = in.u8();
    if (exists > 1) throw std::runtime_error("Invalid body existence flag");
    body.exists = exists != 0;
    body.position = get_position(in);
    body.previous = get_position(in);
    body.half = {get_float(in), get_float(in)};
    body.angle = get_float(in);
    body.previous_angle = get_float(in);
    body.height = get_float(in);
    body.previous_height = get_float(in);
    body.inverse_mass = get_float(in);
    body.health = get_float(in);

    const bool valid_size = body.half.x > 0 && body.half.y > 0 && body.half.x <= 2 && body.half.y <= 2;
    const bool valid_height =
        body.height >= 0 && body.height <= 16 && body.previous_height >= 0 && body.previous_height <= 16;
    const bool valid_angle = std::abs(body.angle) <= 1000000 && std::abs(body.previous_angle) <= 1000000;
    const bool valid_mass = body.inverse_mass >= 0 && body.inverse_mass <= 1;
    const bool valid_health = body.health >= 0 && body.health <= 100 && !(body.exists && body.health == 0);
    if (!valid_size || !valid_height || !valid_angle || !valid_mass || !valid_health ||
        !within_owner_reach(owner, body))
        throw std::runtime_error("Invalid body state");
    return body;
}
bool same_body(const BodyState& a, const BodyState& b) {
    return encode_body(a) == encode_body(b);
}
} // namespace

void check_saved_tile(const WorldGenerator& generator, const Tile& tile) {
    if (!std::isfinite(tile.elevation) || std::abs(tile.elevation) > 1e6F)
        throw std::invalid_argument("Tile elevation must be finite and within 1e6");
    if (generator.materials && (tile.material >= generator.materials ||
                                (tile.object != no_object && tile.object > generator.materials)))
        throw std::invalid_argument("Tile material is not registered");
}

void append_entity(ChunkEntities& entities, const EntityRecord& record) {
    if (entities.count == chunk_entity_capacity) throw std::length_error("Chunk entity budget exhausted");
    if (record.payload.size() > entity_payload_capacity) throw std::length_error("Entity payload too large");
    auto position = record.position;
    position.move({});
    Bytes out;
    put_position(out, position);
    for (float value : {record.angle, record.size.x, record.size.y})
        put_float(out, value);
    out.u8(record.material);
    out.u16(static_cast<std::uint16_t>(record.payload.size()));
    // Validate before committing so a bad record leaves the section unchanged.
    Reader check(out.data);
    get_position(check);
    for (int i = 0; i < 3; ++i)
        get_float(check);
    entities.records.insert(entities.records.end(), out.data.begin(), out.data.end());
    entities.records.insert(entities.records.end(), record.payload.begin(), record.payload.end());
    ++entities.count;
}

EntityRecord read_entity(Reader& in) {
    EntityRecord record;
    record.position = get_position(in);
    record.angle = get_float(in);
    record.size = {get_float(in), get_float(in)};
    record.material = in.u8();
    const auto length = in.u16();
    if (length > entity_payload_capacity) throw std::runtime_error("Entity payload too large");
    record.payload = in.take(length);
    return record;
}

void check_entities(const ChunkEntities& entities) {
    if (entities.count > chunk_entity_capacity) throw std::runtime_error("Invalid entity count");
    each_entity(entities, [](const EntityRecord&) {});
}

std::array<std::uint8_t, body_record_size> encode_body(const BodyState& body) {
    Bytes out;
    out.data.reserve(body_record_size);
    out.u8(body.exists ? 1 : 0);
    put_position(out, body.position);
    put_position(out, body.previous);
    for (float value : {body.half.x, body.half.y, body.angle, body.previous_angle, body.height,
                        body.previous_height, body.inverse_mass, body.health})
        put_float(out, value);
    if (out.data.size() != body_record_size) throw std::logic_error("Body record schema size mismatch");
    std::array<std::uint8_t, body_record_size> result;
    std::copy(out.data.begin(), out.data.end(), result.begin());
    return result;
}

bool chunk_matches_baseline(const Chunk& chunk, const ChunkBodies& baseline) {
    const auto& bodies = chunk.bodies;
    if (chunk.replaced.any() ||
        std::any_of(chunk.changes.begin(), chunk.changes.end(), [](auto change) { return change != 0; }))
        return false;
    if (bodies.count != baseline.count || bodies.broken.any() || chunk.entities.count) return false;
    for (std::size_t i = 0; i < bodies.count; ++i)
        if (!same_body(bodies.bodies[i], baseline.bodies[i])) return false;
    return true;
}

std::vector<std::uint8_t> encode_chunk(const WorldGenerator& generator, std::uint64_t seed, ChunkCoord coord,
                                       const Chunk& chunk, const ChunkBodies& baseline) {
    const auto& bodies = chunk.bodies;
    if (bodies.recipe_count != baseline.recipe_count || bodies.joint_count != baseline.joint_count)
        throw std::logic_error("Chunk bodies do not match their generated recipe");

    Bytes out;
    out.u32(chunk_file_magic);
    out.u32(chunk_file_version);
    out.u64(generator_id(generator));
    out.u32(generator.version);
    out.u64(seed);
    out.u64(std::uint64_t(coord.x));
    out.u64(std::uint64_t(coord.y));

    // Terrain: one record per changed tile, in tile order.
    const auto tile_changes =
        std::count_if(chunk.changes.begin(), chunk.changes.end(), [](auto change) { return change != 0; });
    out.u16(static_cast<std::uint16_t>(tile_changes));
    for (std::size_t i = 0; i < chunk.changes.size(); ++i)
        if (chunk.changes[i]) {
            out.u16(static_cast<std::uint16_t>(i));
            out.u8(chunk.changes[i]);
        }

    // Replaced tiles: each whole, in tile order.
    out.u16(static_cast<std::uint16_t>(chunk.replaced.count()));
    for (std::size_t i = 0; i < chunk.tiles.size(); ++i)
        if (chunk.replaced[i]) {
            const auto& tile = chunk.tiles[i];
            check_saved_tile(generator, tile);
            out.u16(static_cast<std::uint16_t>(i));
            put_float(out, tile.elevation);
            out.u8(tile.material);
            out.u8(tile.flags);
            out.u8(tile.object);
            for (const auto byte : tile.game)
                out.u8(byte);
        }

    // Bodies: changed recipe bodies, then every built body, in ascending ID order.
    std::uint16_t records = 0;
    Bytes body_records;
    for (std::uint16_t i = 0; i < bodies.count; ++i) {
        const bool built = i >= bodies.recipe_count;
        if (!built && same_body(bodies.bodies[i], baseline.bodies[i])) continue;
        if (built && !bodies.bodies[i].exists) throw std::logic_error("Destroyed built bodies are not saved");
        body_records.u16(i);
        const auto encoded = encode_body(bodies.bodies[i]);
        body_records.data.insert(body_records.data.end(), encoded.begin(), encoded.end());
        ++records;
    }
    out.u16(records);
    out.data.insert(out.data.end(), body_records.data.begin(), body_records.data.end());

    // Joints: only broken recipe joints; endpoints and rest lengths are regenerated.
    out.u16(static_cast<std::uint16_t>(bodies.broken.count()));
    for (std::uint16_t i = 0; i < bodies.joint_count; ++i)
        if (bodies.broken[i]) out.u16(i);

    // Entities: the saved records verbatim.
    out.u16(chunk.entities.count);
    out.u32(static_cast<std::uint32_t>(chunk.entities.records.size()));
    out.data.insert(out.data.end(), chunk.entities.records.begin(), chunk.entities.records.end());
    return out.data;
}

void decode_chunk(std::span<const std::uint8_t> bytes, const WorldGenerator& generator, std::uint64_t seed,
                  ChunkCoord coord, Chunk& chunk) {
    Reader in(bytes);
    if (in.u32() != chunk_file_magic) throw std::runtime_error("Invalid chunk file magic");
    const auto version = in.u32();
    if (version != chunk_file_version)
        throw std::runtime_error(
            "Unsupported chunk file version; legacy migration is disabled. Choose a new save directory");
    if (in.u64() != generator_id(generator) || in.u32() != generator.version || in.u64() != seed ||
        in.u64() != std::uint64_t(coord.x) || in.u64() != std::uint64_t(coord.y))
        throw std::runtime_error("Chunk file belongs to another generator, seed or coordinate");

    const auto tile_changes = in.u16();
    if (tile_changes > chunk.tiles.size()) throw std::runtime_error("Invalid tile change count");
    for (unsigned i = 0; i < tile_changes; ++i) {
        const auto index = in.u16();
        const auto change = in.u8();
        if (index >= chunk.tiles.size() || !change || (change & ~generator.edit_bits) || chunk.changes[index])
            throw std::runtime_error("Invalid or duplicate tile change");
        chunk.changes[index] = change;
        generator.apply_edit(generator.context, chunk.tiles[index], change);
    }

    const auto replaced = in.u16();
    if (replaced > chunk.tiles.size()) throw std::runtime_error("Invalid replaced tile count");
    int previous_tile = -1;
    for (unsigned i = 0; i < replaced; ++i) {
        const auto index = in.u16();
        if (index >= chunk.tiles.size() || static_cast<int>(index) <= previous_tile)
            throw std::runtime_error("Replaced tiles out of order or out of range");
        previous_tile = index;
        Tile tile;
        tile.elevation = std::bit_cast<float>(in.u32());
        tile.material = in.u8();
        tile.flags = in.u8();
        tile.object = in.u8();
        for (auto& byte : tile.game)
            byte = in.u8();
        try {
            check_saved_tile(generator, tile);
        } catch (const std::invalid_argument& error) {
            throw std::runtime_error(std::string("Invalid replaced tile: ") + error.what());
        }
        chunk.tiles[index] = tile;
        chunk.replaced.set(index);
    }

    auto& bodies = chunk.bodies;
    const auto records = in.u16();
    if (records > chunk_body_capacity) throw std::runtime_error("Invalid body record count");
    int previous_id = -1;
    for (unsigned n = 0; n < records; ++n) {
        const auto id = in.u16();
        if (static_cast<int>(id) <= previous_id) throw std::runtime_error("Body records out of order");
        previous_id = id;
        const auto body = decode_body(in, coord);
        if (id < bodies.recipe_count) {
            bodies.bodies[id] = body;
            continue;
        }
        // Built bodies are stored densely right after the recipe and always exist.
        if (id != bodies.count || id >= chunk_body_capacity || !body.exists)
            throw std::runtime_error("Invalid built body record");
        bodies.bodies[bodies.count++] = body;
    }

    const auto broken = in.u16();
    if (broken > bodies.joint_count) throw std::runtime_error("Invalid broken joint count");
    for (unsigned n = 0; n < broken; ++n) {
        const auto id = in.u16();
        if (id >= bodies.joint_count || bodies.broken[id]) throw std::runtime_error("Invalid broken joint");
        bodies.broken.set(id);
    }

    ChunkEntities entities;
    entities.count = in.u16();
    const auto section = in.take(in.u32());
    entities.records.assign(section.begin(), section.end());
    check_entities(entities);
    chunk.entities = std::move(entities);
    if (!in.done()) throw std::runtime_error("Trailing chunk data");
}
} // namespace seed
