#pragma once
#include "core/ecs.hpp"
#include "core/material.hpp"
#include "core/math.hpp"
#include "world/coordinates.hpp"
#include <memory>

namespace seed {
struct Transform {
    WorldPosition position{}, previous{};
    float angle{};
};
struct Visual {
    MaterialId material{};
    Vec2 size{0.7F, 0.7F};
};

// Entities with the engine's Transform and Visual components, plus any component types the game
// registers. Every pool lives in one arena allocated up front, so no component storage touches the
// general-purpose heap after startup.
class Scene final {
public:
    static constexpr std::size_t default_memory = 2 * 1024 * 1024;
    static constexpr std::size_t component_capacity = 16; // Game component types.

    explicit Scene(std::size_t memory_bytes = default_memory)
        : memory_(memory_bytes), entities_(memory_), transforms(memory_), visuals(memory_) {}
    Entity create(Transform transform, Visual visual) {
        const auto entity = entities_.create();
        transforms.add(entity, transform);
        visuals.add(entity, visual);
        return entity;
    }
    void destroy(Entity e) {
        if (!entities_.alive(e)) throw std::out_of_range("Destroy stale scene entity");
        transforms.remove(e);
        visuals.remove(e);
        for (std::size_t i = 0; i < pool_count_; ++i)
            pools_[i].remove(pools_[i].set, e);
        entities_.destroy(e);
    }
    bool alive(Entity e) const { return entities_.alive(e); }

    // Registers a game component type and returns its dense pool. Components must be trivially
    // copyable. Registering a type twice is an error; use components<T>() to reach it again.
    template<class T>
    SparseSet<T>& add_component() {
        if (find_pool(key<T>())) throw std::logic_error("Component type registered twice");
        if (pool_count_ == component_capacity) throw std::length_error("Too many component types");
        auto* set = std::construct_at(
            static_cast<SparseSet<T>*>(memory_.allocate(sizeof(SparseSet<T>), alignof(SparseSet<T>))),
            memory_);
        pools_[pool_count_++] = {key<T>(), set, [](void* pool, Entity e) {
                                     static_cast<SparseSet<T>*>(pool)->remove(e);
                                 }};
        return *set;
    }
    template<class T>
    SparseSet<T>& components() {
        auto* pool = find_pool(key<T>());
        if (!pool) throw std::logic_error("Component type was never registered");
        return *static_cast<SparseSet<T>*>(pool->set);
    }
    std::size_t memory_used() const { return memory_.used(); }

private:
    struct Pool {
        const void* type{};
        void* set{};
        void (*remove)(void*, Entity){};
    };
    // One address per component type identifies its pool without RTTI.
    template<class T>
    static const void* key() {
        static const char tag{};
        return &tag;
    }
    Pool* find_pool(const void* type) {
        for (std::size_t i = 0; i < pool_count_; ++i)
            if (pools_[i].type == type) return &pools_[i];
        return nullptr;
    }
    Arena memory_;
    Entities entities_;
    std::array<Pool, component_capacity> pools_{};
    std::size_t pool_count_{};

public:
    SparseSet<Transform> transforms;
    SparseSet<Visual> visuals;
};
} // namespace seed
