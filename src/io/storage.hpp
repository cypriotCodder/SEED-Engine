#pragma once
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace seed {
void sync_directory(const std::filesystem::path& path);
// Checksummed LZ4 envelope; the caller owns the versioned payload schema.
std::vector<std::uint8_t> read_blob(const std::filesystem::path& path);
void write_blob(const std::filesystem::path& path, std::span<const std::uint8_t> bytes);
} // namespace seed
