#pragma once
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
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
// Replaces a file atomically: writes a temporary beside it, flushes, then renames over it.
void replace_file(const std::filesystem::path& path, std::span<const std::uint8_t> bytes);
void write_text(const std::filesystem::path& path, std::string_view text);
// Reads a whole file; throws if it is missing, unreadable or larger than `limit` bytes.
std::string read_text(const std::filesystem::path& path, std::size_t limit = 16 * 1024 * 1024);
} // namespace seed
