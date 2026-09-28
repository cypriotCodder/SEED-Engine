#include "project/characters.hpp"
#include "app/app.hpp"
#include "physics/character.hpp"
#include "project/scene_file.hpp"
#include <cmath>

namespace seed {
namespace {
bool blocked(void* context, const Tile* tile) {
    const auto& c = *static_cast<const CharacterMotion*>(context);
    return !tile || (c.solid && (tile->flags & tile_solid)) || (c.water && tile->elevation < 0);
}
float length_of(Vec2 v) {
    return std::sqrt(v.x * v.x + v.y * v.y);
}
} // namespace

CharacterMotion character_motion(const SceneCharacter& settings) {
    CharacterMotion motion;
    motion.speed = settings.speed;
    motion.run_speed = settings.run_speed;
    motion.acceleration = settings.acceleration;
    motion.half = settings.collision * 0.5F;
    motion.water = settings.water;
    motion.solid = settings.solid;
    motion.buildings = settings.buildings;
    motion.face_movement = settings.face_movement;
    return motion;
}

void move_characters(Engine& engine, float dt) {
    auto& motions = engine.scene.components<CharacterMotion>();
    const auto owners = motions.owners();
    const auto values = motions.values();
    for (std::size_t i = 0; i < owners.size(); ++i) {
        auto& m = values[i];
        auto* transform = engine.scene.transforms.find(owners[i]);
        if (!transform) continue;
        const float top = m.running ? m.run_speed : m.speed;
        Vec2 direction = m.walk;
        float limit = top * dt; // How far this step may go.
        bool arriving = false;
        if (m.has_target) {
            if (!nearby(m.target.chunk, transform->position.chunk, 64)) {
                m.has_target = false; // Too far to aim at.
                direction = {};
            } else {
                const auto to = relative(m.target, transform->position);
                const float distance = length_of(to);
                if (distance < 1e-3F) {
                    m.has_target = false;
                    direction = {};
                } else {
                    direction = to * (1 / distance);
                    arriving = distance <= limit;
                    limit = std::min(limit, distance); // Stop on the target, not past it.
                }
            }
        }
        if (const float l = length_of(direction); l > 1) direction = direction * (1 / l);
        const Vec2 wanted = direction * top;
        if (m.acceleration <= 0)
            m.velocity = wanted;
        else {
            // Turn and speed up or slow down at the character's acceleration.
            const auto change = wanted - m.velocity;
            const float step = m.acceleration * dt, needed = length_of(change);
            m.velocity = needed <= step ? wanted : m.velocity + change * (step / needed);
        }
        auto delta = m.velocity * dt;
        if (const float l = length_of(delta); l > limit && l > 0) delta = delta * (limit / l);
        if (delta.x == 0 && delta.y == 0) continue;
        const auto before = transform->position;
        transform->position = move_character(engine.world, engine.physics, transform->position, delta, m.half,
                                             {&m, blocked, m.buildings});
        // Bumping into something takes the speed out of that direction.
        const auto moved = relative(transform->position, before);
        if (std::abs(moved.x) < std::abs(delta.x) * 0.5F) m.velocity.x = 0;
        if (std::abs(moved.y) < std::abs(delta.y) * 0.5F) m.velocity.y = 0;
        // A step meant to reach the target lands exactly on it, unless something was in the way.
        if (arriving && length_of(relative(m.target, transform->position)) < 1e-3F) {
            transform->position = m.target;
            m.has_target = false;
            m.velocity = {};
        }
        if (m.face_movement && length_of(moved) > 1e-4F) transform->angle = std::atan2(moved.y, moved.x);
    }
}
} // namespace seed
