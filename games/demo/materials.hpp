#pragma once
#include "core/material.hpp"

namespace demo {
// The demo's materials, in registration order. Their IDs double as atlas slots, so the order is
// part of how the demo looks: append new materials at the end.
namespace mat {
enum : seed::MaterialId {
    water,
    sand,
    grass,
    stone,
    wood,
    leaves,
    player,
    ember,
    deep_water,
    forest_floor,
    mud,
    dry_grass,
    snow,
};
} // namespace mat

inline void register_materials(void*, seed::Materials& materials) {
    using seed::Pattern;
    constexpr seed::MaterialDesc table[] = {
        {"water", {29, 76, 104}, Pattern::water, 23, nullptr},
        {"sand", {191, 172, 110}, Pattern::speckle, 23, nullptr},
        {"grass", {73, 111, 57}, Pattern::speckle, 23, nullptr},
        {"stone", {119, 126, 123}, Pattern::speckle, 23, nullptr},
        {"wood", {131, 85, 49}, Pattern::planks, 23, nullptr},
        {"leaves", {42, 91, 44}, Pattern::round, 23, nullptr},
        {"player", {215, 167, 93}, Pattern::round, 23, nullptr},
        {"ember", {255, 166, 45}, Pattern::round, 23, "flame"}, // Drawn from the packed flame texture.
        {"deep_water", {17, 50, 79}, Pattern::water, 15, nullptr},
        {"forest_floor", {47, 74, 38}, Pattern::speckle, 27, nullptr},
        {"mud", {82, 71, 47}, Pattern::speckle, 19, nullptr},
        {"dry_grass", {158, 148, 82}, Pattern::speckle, 25, nullptr},
        {"snow", {226, 232, 238}, Pattern::speckle, 9, nullptr},
    };
    seed::MaterialId expected = 0;
    for (const auto& desc : table)
        if (materials.add(desc) != expected++)
            throw std::logic_error("Demo materials must be registered first");
}
} // namespace demo
