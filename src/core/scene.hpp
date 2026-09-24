#pragma once
#include "core/ecs.hpp"
#include "core/math.hpp"
#include "world/coordinates.hpp"
#include "render/renderer.hpp"

namespace seed {
struct Transform { WorldPosition position{}, previous{}; float angle{}; };
struct Visual { Material material{Material::player}; Vec2 size{0.7F,0.7F}; };
class Scene final {
public:
    Scene() : memory_(2*1024*1024), entities_(memory_), transforms(memory_), visuals(memory_) {}
    Entity create(Transform transform, Visual visual) {
        const auto entity=entities_.create();
        transforms.add(entity,transform); visuals.add(entity,visual); return entity;
    }
    void destroy(Entity e) {
        if (!entities_.alive(e)) throw std::out_of_range("Destroy stale scene entity");
        transforms.remove(e); visuals.remove(e); entities_.destroy(e);
    }
    bool alive(Entity e) const { return entities_.alive(e); }
private:
    Arena memory_;
    Entities entities_;
public:
    SparseSet<Transform> transforms;
    SparseSet<Visual> visuals;
};
} // namespace seed
