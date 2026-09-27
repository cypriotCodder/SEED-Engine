#pragma once
#include "core/math.hpp"
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace seed {
constexpr int chunk_side = 32;
struct ChunkCoord {
    std::int64_t x{}, y{};
    bool operator==(const ChunkCoord&) const = default;
};
inline std::int64_t checked_add(std::int64_t a, std::int64_t b) {
    if ((b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) ||
        (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b))
        throw std::overflow_error("World coordinate boundary");
    return a + b;
}
struct WorldPosition {
    ChunkCoord chunk{};
    Vec2 local{};
    bool operator==(const WorldPosition&) const = default;
    void move(Vec2 movement) {
        auto normalize = [](std::int64_t& c, float& p) {
            if (!std::isfinite(p) || std::abs(p) > 1048576)
                throw std::out_of_range("Oversized world movement");
            const auto shift = static_cast<std::int64_t>(std::floor(p / chunk_side));
            c = checked_add(c, shift);
            p -= static_cast<float>(shift) * chunk_side;
        };
        auto copy = *this;
        copy.local += movement;
        normalize(copy.chunk.x, copy.local.x);
        normalize(copy.chunk.y, copy.local.y);
        *this = copy;
    }
};
// Use only for the bounded active neighborhood. Unsigned subtraction avoids signed overflow.
inline float chunk_distance(std::int64_t a, std::int64_t b) {
    const bool negative = a < b;
    const auto magnitude =
        negative ? std::uint64_t(b) - std::uint64_t(a) : std::uint64_t(a) - std::uint64_t(b);
    if (magnitude > 1024) throw std::out_of_range("Positions are outside one simulation neighborhood");
    return (negative ? -1.0F : 1.0F) * static_cast<float>(magnitude) * chunk_side;
}
inline Vec2 relative(WorldPosition a, WorldPosition b) {
    return {chunk_distance(a.chunk.x, b.chunk.x) + a.local.x - b.local.x,
            chunk_distance(a.chunk.y, b.chunk.y) + a.local.y - b.local.y};
}
inline bool nearby(ChunkCoord a, ChunkCoord b, std::uint64_t radius) {
    auto distance = [](std::int64_t x, std::int64_t y) {
        return x < y ? std::uint64_t(y) - std::uint64_t(x) : std::uint64_t(x) - std::uint64_t(y);
    };
    return distance(a.x, b.x) <= radius && distance(a.y, b.y) <= radius;
}
} // namespace seed
