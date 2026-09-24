#pragma once
#include "world/generator.hpp"
#include <cstdint>
#include <span>
#include <vector>

namespace seed {
constexpr std::uint32_t chunk_file_magic = 0x4b4e4843;
constexpr std::uint32_t chunk_file_version = 2;
constexpr std::size_t body_record_size = 81;

// Fixed-size little-endian encoding of one body, used both for saving and for change detection.
std::array<std::uint8_t, body_record_size> encode_body(const BodyState& body);

// Serializes the chunk's differences from its generated baseline. `baseline` must be the
// freshly generated structure recipe of the same chunk.
std::vector<std::uint8_t> encode_chunk(std::uint64_t seed, ChunkCoord coord, const Chunk& chunk,
                                       const ChunkBodies& baseline);

// Applies a saved delta to a chunk that has just been generated for the same seed and coordinate.
// Throws on any malformed, duplicate, out-of-range or trailing data.
void decode_chunk(std::span<const std::uint8_t> bytes, std::uint64_t seed, ChunkCoord coord, Chunk& chunk);

// Imports the previous standalone version-2 building delta into the origin chunk.
void decode_legacy_bodies(std::span<const std::uint8_t> bytes, std::uint64_t seed, Chunk& chunk);

// True when the chunk carries no terrain or building differences from its baseline.
bool chunk_matches_baseline(const Chunk& chunk, const ChunkBodies& baseline);
} // namespace seed
