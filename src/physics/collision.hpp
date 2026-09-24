#pragma once
#include "core/math.hpp"
#include <array>
#include <limits>

namespace seed {
struct Aabb {
    Vec2 minimum{}, maximum{};
};
inline bool overlaps(Aabb a, Aabb b) {
    return a.minimum.x < b.maximum.x && a.maximum.x > b.minimum.x && a.minimum.y < b.maximum.y &&
           a.maximum.y > b.minimum.y;
}
struct Box {
    Vec2 center{}, half{0.5F, 0.5F};
    float angle{};
};
inline Aabb bounds(Box b) {
    const auto x = rotate({b.half.x, 0}, b.angle), y = rotate({0, b.half.y}, b.angle);
    const Vec2 extent{std::abs(x.x) + std::abs(y.x), std::abs(x.y) + std::abs(y.y)};
    return {b.center - extent, b.center + extent};
}
struct Contact {
    Vec2 normal{}, point{};
    float depth{};
};
inline bool collide(Box a, Box b, Contact& contact) {
    if (!overlaps(bounds(a), bounds(b))) return false;
    const std::array<Vec2, 4> axes{rotate({1, 0}, a.angle), rotate({0, 1}, a.angle), rotate({1, 0}, b.angle),
                                   rotate({0, 1}, b.angle)};
    float depth = std::numeric_limits<float>::max();
    Vec2 normal;
    const auto delta = b.center - a.center;
    for (auto axis : axes) {
        const float ra = std::abs(dot(axis, axes[0])) * a.half.x + std::abs(dot(axis, axes[1])) * a.half.y;
        const float rb = std::abs(dot(axis, axes[2])) * b.half.x + std::abs(dot(axis, axes[3])) * b.half.y;
        const float overlap = ra + rb - std::abs(dot(delta, axis));
        if (overlap <= 0) return false;
        if (overlap < depth) {
            depth = overlap;
            normal = dot(delta, axis) < 0 ? axis * (-1) : axis;
        }
    }
    auto support = [](Box box, Vec2 direction) {
        const auto local = rotate(direction, -box.angle);
        auto coordinate = [](float direction, float extent) {
            return std::abs(direction) < 1e-5F ? 0.0F : (direction > 0 ? extent : -extent);
        };
        return box.center +
               rotate({coordinate(local.x, box.half.x), coordinate(local.y, box.half.y)}, box.angle);
    };
    contact = {normal, (support(a, normal) + support(b, normal * (-1))) * 0.5F, depth};
    return true;
}
inline float cross(Vec2 a, Vec2 b) {
    return a.x * b.y - a.y * b.x;
}
} // namespace seed
