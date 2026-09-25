#include "io/storage.hpp"
#include "io/binary.hpp"
#include <fstream>
#include <limits>
#include <lz4.h>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace seed {
namespace {
constexpr std::uint32_t magic = 0x344c4453, maximum = 64 * 1024 * 1024;
void sync_file(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open save for sync");
    const auto ok = FlushFileBuffers(handle);
    CloseHandle(handle);
    if (!ok) throw std::runtime_error("Cannot sync save");
#else
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) throw std::runtime_error("Cannot open save for sync");
    const int result = ::fsync(fd);
    ::close(fd);
    if (result != 0) throw std::runtime_error("Cannot sync save");
#endif
}
} // namespace
bool snapshot_blob(const std::filesystem::path& source, const std::filesystem::path& target,
                   bool allow_link) {
    if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(source)))
        throw std::runtime_error("Snapshot source must be a regular file");
    if (allow_link) {
        std::error_code error;
        std::filesystem::create_hard_link(source, target, error);
        if (!error) return true;
        // Some filesystems do not support hard links. Copy the compressed bytes unchanged.
    }
    std::filesystem::copy_file(source, target);
    sync_file(target);
    return false;
}
void sync_directory(const std::filesystem::path& path) {
#ifndef _WIN32
    sync_file(path);
#else
    // File replacement uses MOVEFILE_WRITE_THROUGH on Windows.
    (void)path;
#endif
}
std::vector<std::uint8_t> read_blob(const std::filesystem::path& path) {
    const auto length = std::filesystem::file_size(path);
    if (length < 20 || length > maximum + 20) throw std::runtime_error("Invalid compressed file size");
    std::vector<std::uint8_t> file(static_cast<std::size_t>(length));
    std::ifstream input(path, std::ios::binary);
    input.read(reinterpret_cast<char*>(file.data()), static_cast<std::streamsize>(file.size()));
    if (!input) throw std::runtime_error("Cannot read compressed file");
    Reader header(file);
    if (header.u32() != magic || header.u32() != 1) throw std::runtime_error("Unknown compression envelope");
    const auto raw = header.u32(), packed = header.u32(), checksum = header.u32();
    if (!raw || raw > maximum || packed != file.size() - 20)
        throw std::runtime_error("Invalid compressed lengths");
    std::vector<std::uint8_t> result(raw);
    const int decoded = LZ4_decompress_safe(reinterpret_cast<const char*>(file.data() + 20),
                                            reinterpret_cast<char*>(result.data()), static_cast<int>(packed),
                                            static_cast<int>(raw));
    if (decoded != static_cast<int>(raw) || crc32(result) != checksum)
        throw std::runtime_error("Corrupt compressed file");
    return result;
}
void write_blob(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    if (bytes.empty() || bytes.size() > maximum)
        throw std::runtime_error("Unsupported compressed payload size");
    const int bound = LZ4_compressBound(static_cast<int>(bytes.size()));
    std::vector<char> compressed(static_cast<std::size_t>(bound));
    const int count = LZ4_compress_default(reinterpret_cast<const char*>(bytes.data()), compressed.data(),
                                           static_cast<int>(bytes.size()), bound);
    if (count <= 0 || count > static_cast<int>(maximum)) throw std::runtime_error("LZ4 compression failed");
    Bytes header;
    header.u32(magic);
    header.u32(1);
    header.u32(static_cast<std::uint32_t>(bytes.size()));
    header.u32(static_cast<std::uint32_t>(count));
    header.u32(crc32(bytes));
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(header.data.data()),
                     static_cast<std::streamsize>(header.data.size()));
        output.write(compressed.data(), count);
        output.flush();
        if (!output) throw std::runtime_error("Cannot write compressed file: " + temporary.string());
        output.close();
        if (!output) throw std::runtime_error("Cannot close compressed file");
    }
    sync_file(temporary);
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot replace compressed file");
#else
    std::filesystem::rename(temporary, path);
    const auto parent = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
    sync_file(parent);
#endif
}
} // namespace seed
