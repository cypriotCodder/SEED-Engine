#pragma once
#include "core/material.hpp"
#include "io/binary.hpp"
#include "world/world_generator.hpp"
#include <cstdint>
#include <span>
#include <vector>

namespace seed {
constexpr std::uint32_t chunk_file_magic = 0x4b4e4843;
constexpr std::uint32_t chunk_file_version = 4;
constexpr std::size_t body_record_size = 81;

// One saved entity: where it is, how it looks, and the game's own bytes for its other components.
struct EntityRecord {
    WorldPosition position{};
    float angle{};
    MaterialId material{};
    Vec2 size{};
    std::span<const std::uint8_t> payload; // At most entity_payload_capacity bytes.
};
// Appends one record to a chunk's entity section.
void append_entity(ChunkEntities& entities, const EntityRecord& record);
// Calls visit(record) for each record in order. Throws on malformed data; `payload` points into
// `entities` and is valid only during the call.
template<class F>
void each_entity(const ChunkEntities& entities, F&& visit);
// Validates the structure of every record without visiting them.
void check_entities(const ChunkEntities& entities);
EntityRecord read_entity(Reader& in);

// Fixed-size little-endian encoding of one body, used both for saving and for change detection.
std::array<std::uint8_t, body_record_size> encode_body(const BodyState& body);

// Serializes the chunk's differences from its generated baseline. `baseline` must be the
// freshly generated structure recipe of the same chunk.
std::vector<std::uint8_t> encode_chunk(const WorldGenerator& generator, std::uint64_t seed, ChunkCoord coord,
                                       const Chunk& chunk, const ChunkBodies& baseline);

// Applies a saved delta to a chunk that has just been generated for the same seed and coordinate.
// Throws on any malformed, duplicate, out-of-range or trailing data.
void decode_chunk(std::span<const std::uint8_t> bytes, const WorldGenerator& generator, std::uint64_t seed,
                  ChunkCoord coord, Chunk& chunk);

// True when the chunk carries no terrain or building differences from its baseline.
bool chunk_matches_baseline(const Chunk& chunk, const ChunkBodies& baseline);

template<class F>
void each_entity(const ChunkEntities& entities, F&& visit) {
    Reader in(entities.records);
    for (unsigned i = 0; i < entities.count; ++i)
        visit(read_entity(in));
    if (!in.done()) throw std::runtime_error("Trailing entity data");
}
} // namespace seed
