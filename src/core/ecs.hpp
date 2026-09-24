#pragma once
#include "core/memory.hpp"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>

namespace seed {
struct Entity {
    std::uint32_t index{}, generation{};
    bool operator==(const Entity&) const = default;
};
constexpr std::uint32_t entity_capacity = 8192; // Building bodies (4,096) plus everything else.
constexpr auto absent = std::numeric_limits<std::uint32_t>::max();

class Entities final {
public:
    Entities(const Entities&) = delete;
    Entities& operator=(const Entities&) = delete;
    explicit Entities(Arena& arena)
        : generations_(arena.array<std::uint32_t>(entity_capacity)),
          next_(arena.array<std::uint32_t>(entity_capacity)),
          alive_(arena.array<bool>(entity_capacity)) {
        for (std::uint32_t i = 0; i < entity_capacity; ++i) {
            generations_[i] = 1;
            next_[i] = i + 1;
        }
    }
    Entity create() {
        if (free_ >= entity_capacity) throw std::bad_alloc();
        const auto i = free_;
        free_ = next_[i];
        alive_[i] = true;
        return {i, generations_[i]};
    }
    bool alive(Entity e) const {
        return e.index < entity_capacity && alive_[e.index] && generations_[e.index] == e.generation;
    }
    void destroy(Entity e) {
        if (!alive(e)) throw std::out_of_range("Stale entity handle");
        alive_[e.index] = false;
        // Retire a slot on generation exhaustion rather than resurrecting an old handle.
        if (generations_[e.index] != absent) {
            ++generations_[e.index];
            next_[e.index] = free_;
            free_ = e.index;
        }
    }

private:
    std::uint32_t *generations_, *next_;
    bool* alive_;
    std::uint32_t free_{};
};

template<class T>
class SparseSet final {
public:
    static_assert(std::is_trivially_copyable_v<T>, "Dense components must be trivially copyable");
    SparseSet(const SparseSet&) = delete;
    SparseSet& operator=(const SparseSet&) = delete;
    explicit SparseSet(Arena& arena)
        : sparse_(arena.array<std::uint32_t>(entity_capacity)),
          owners_(arena.array<Entity>(entity_capacity)),
          data_(arena.array<T>(entity_capacity)) {
        std::fill_n(sparse_, entity_capacity, absent);
    }
    T* find(Entity e) {
        if (e.index >= entity_capacity) return nullptr;
        const auto i = sparse_[e.index];
        return i < size_ && owners_[i] == e ? data_ + i : nullptr;
    }
    T& add(Entity e, const T& value = {}) {
        if (e.index >= entity_capacity) throw std::out_of_range("Entity index");
        if (sparse_[e.index] != absent) throw std::logic_error("Entity already owns this component");
        if (size_ == entity_capacity) throw std::bad_alloc();
        sparse_[e.index] = size_;
        owners_[size_] = e;
        data_[size_] = value;
        return data_[size_++];
    }
    void remove(Entity e) {
        if (!find(e)) return;
        const auto index = sparse_[e.index];
        --size_;
        if (index != size_) {
            data_[index] = data_[size_];
            owners_[index] = owners_[size_];
            sparse_[owners_[index].index] = index;
        }
        sparse_[e.index] = absent;
    }
    std::span<T> values() { return {data_, size_}; }
    std::span<const Entity> owners() const { return {owners_, size_}; }

private:
    std::uint32_t* sparse_;
    Entity* owners_;
    T* data_;
    std::uint32_t size_{};
};
} // namespace seed
