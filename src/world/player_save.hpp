#pragma once
#include "io/binary.hpp"
#include "io/storage.hpp"
#include "world/coordinates.hpp"
#include <bit>

namespace seed {
inline WorldPosition load_player(const std::filesystem::path& directory, std::uint64_t seed,
                                 std::uint32_t generator_version) {
    const auto file = directory / "player.delta";
    if (!std::filesystem::exists(file)) return {};
    const auto bytes = read_blob(file);
    Reader input(bytes);
    if (input.u32() != 0x52594c50 || input.u32() != 1 || input.u32() != generator_version ||
        input.u64() != seed)
        throw std::runtime_error("Unsupported player save");
    WorldPosition result;
    result.chunk = {std::bit_cast<std::int64_t>(input.u64()), std::bit_cast<std::int64_t>(input.u64())};
    result.local = {std::bit_cast<float>(input.u32()), std::bit_cast<float>(input.u32())};
    if (!std::isfinite(result.local.x) || !std::isfinite(result.local.y) || result.local.x < 0 ||
        result.local.y < 0 || result.local.x >= chunk_side || result.local.y >= chunk_side || !input.done())
        throw std::runtime_error("Invalid player coordinates");
    return result;
}
inline void save_player(const std::filesystem::path& directory, std::uint64_t seed,
                        std::uint32_t generator_version, WorldPosition position,
                        bool previously_saved = false) {
    position.move({});
    if (position.chunk == ChunkCoord{} && position.local.x == 0 && position.local.y == 0 &&
        !previously_saved && !std::filesystem::exists(directory / "player.delta"))
        return;
    Bytes output;
    output.u32(0x52594c50);
    output.u32(1);
    output.u32(generator_version);
    output.u64(seed);
    output.u64(std::uint64_t(position.chunk.x));
    output.u64(std::uint64_t(position.chunk.y));
    output.u32(std::bit_cast<std::uint32_t>(position.local.x));
    output.u32(std::bit_cast<std::uint32_t>(position.local.y));
    write_blob(directory / "player.delta", output.data);
}
} // namespace seed
