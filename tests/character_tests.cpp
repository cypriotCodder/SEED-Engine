#include "physics/character.hpp"
#include "test_world.hpp"
#include <filesystem>
#include <iostream>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
// Tiles marked by edit bit 1 (game byte 0) block, as a game-specific rule would.
bool marked(void*, const seed::Tile* tile) {
    return !tile || tile->game[0] == 1;
}
} // namespace

int main() {
    try {
        const auto dir = std::filesystem::temp_directory_path() / "seed-engine-character-test";
        std::filesystem::remove_all(dir);
        seed::Jobs jobs(2);
        seed::Scene scene;
        seed::Physics physics(scene, jobs);
        seed::World world(jobs, seed::stable_id("character-test"), test_world::generator(), 3, dir);
        world.observe(physics.hooks());
        world.settle({});

        // A wall of marked tiles at x = 10 in chunk (0, 0).
        for (int y = 0; y < seed::chunk_side; ++y)
            check(world.edit({{}, {10.5F, static_cast<float>(y) + 0.5F}}, 1), "Mark a wall tile");
        const seed::TileRule rule{nullptr, marked};
        const seed::Vec2 half{0.25F, 0.25F};

        // Free movement goes all the way.
        const auto free = seed::move_character(world, physics, {{}, {5, 12}}, {1, 1}, half, rule);
        check(free.local.x == 6 && free.local.y == 13, "Unobstructed move");

        // Moving diagonally into the wall stops x at the wall but keeps sliding along y.
        const auto slide = seed::move_character(world, physics, {{}, {9.6F, 12}}, {0.3F, 0.3F}, half, rule);
        check(slide.local.x == 9.6F && slide.local.y == 12.3F, "Slide along a tile wall");

        // Without the game's rule, marked tiles are ordinary ground.
        const auto through = seed::move_character(world, physics, {{}, {9.6F, 12}}, {0.3F, 0}, half);
        check(through.local.x > 9.6F, "Default rule ignores game-defined marks");

        // The test platform's plank at (0, 3) spans y 2.76-3.24: walking north into it stops.
        const auto plank = seed::move_character(world, physics, {{}, {0, 2.4F}}, {0, 0.2F}, half, rule);
        check(plank.local.y == 2.4F, "Building bodies block characters");
        check(!seed::box_blocked(world, physics, {{}, {0, 2.4F}}, half, rule), "Clear space is not blocked");

        // Unloaded ground blocks under both rules.
        check(seed::box_blocked(world, physics, {{40, 40}, {5, 5}}, half), "Unloaded tiles block");
        std::filesystem::remove_all(dir);
        std::cout << "Character movement and collision query checks passed.\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
