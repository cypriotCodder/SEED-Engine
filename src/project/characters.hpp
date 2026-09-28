#pragma once
#include "core/ecs.hpp"
#include "world/coordinates.hpp"

namespace seed {
class Engine;
struct SceneCharacter;

// A walking entity's state: the player or an NPC. Whoever controls it (the keys, or a script)
// only sets where it wants to go; move_characters moves every character by the same rules.
struct CharacterMotion {
    float speed{5}, run_speed{8};                   // Tiles per second.
    float acceleration{};                           // Tiles per second squared; 0 reaches full speed at once.
    Vec2 half{0.3F, 0.3F};                          // Half the collision box.
    bool water{true}, solid{true}, buildings{true}; // What blocks it.
    bool face_movement{};
    // Intent, set each step or kept until changed: a direction of length at most 1, or a target.
    Vec2 walk{};
    bool running{};
    bool has_target{};
    WorldPosition target{};
    Vec2 velocity{}; // Tiles per second, as last moved.
};

CharacterMotion character_motion(const SceneCharacter& settings);

// Moves every entity with a CharacterMotion one step of `dt` seconds, sliding along whatever
// blocks it. Physics must be idle (call from Game::step). Characters that reach their target stop.
void move_characters(Engine& engine, float dt);
} // namespace seed
