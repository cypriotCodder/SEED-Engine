#pragma once
#include "core/ecs.hpp"
#include "world/coordinates.hpp"
#include <cstdint>

namespace seed {
class Engine;
struct SceneCharacter;

// A scene area at run time: half its size. See SceneArea.
struct AreaComponent {
    Vec2 half{};
};

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
    // The ground material under its centre, and whether that changed in the last move; none
    // until it first stands on a loaded tile. Placed here, these fill padding before `target`:
    // every scene reserves room for 8,192 of these.
    static constexpr std::int16_t no_surface = -1;
    std::int16_t surface{no_surface};
    bool surface_changed{};
    // An animated visual plays while it walks and shows its first frame when it stands; off once a
    // script picks frames itself (e:set_frame).
    bool animate{true};
    WorldPosition target{};
    Vec2 velocity{}; // Tiles per second, as last moved.
};
static_assert(sizeof(CharacterMotion) <= 72, "CharacterMotion grew; scenes reserve 8,192 of them");

CharacterMotion character_motion(const SceneCharacter& settings);

// Moves every entity with a CharacterMotion one step of `dt` seconds, sliding along whatever
// blocks it. Each walks at its speed times the speed of the ground material under it. Physics
// must be idle (call from Game::step). Characters that reach their target stop.
void move_characters(Engine& engine, float dt);
// Sets each animating character's visual to play while it moves and hold its first frame when not.
void animate_characters(Engine& engine);
} // namespace seed
