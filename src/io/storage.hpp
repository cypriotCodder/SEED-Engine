#pragma once
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace seed {
// Snapshot a blob without recompression. Links are safe only because write_blob replaces inodes.
// Returns true for a hard link, false for a durable byte-for-byte copy.
bool snapshot_blob(const std::filesystem::path& source, const std::filesystem::path& target,
                   bool allow_link = true);
void sync_directory(const std::filesystem::path& path);
// Checksummed LZ4 envelope; the caller owns the versioned payload schema.
std::vector<std::uint8_t> read_blob(const std::filesystem::path& path);
void write_blob(const std::filesystem::path& path, std::span<const std::uint8_t> bytes);
} // namespace seed
