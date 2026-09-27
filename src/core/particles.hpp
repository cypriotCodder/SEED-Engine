#pragma once
#include "core/memory.hpp"
#include "render/renderer.hpp"
#include "world/noise.hpp"
#include <string>
#include <string_view>

namespace seed {
using ParticleStyleId = std::uint8_t;

// How one kind of burst looks and moves. Particles fly out in random directions, slow down, spin
// and shrink to nothing over their life.
struct ParticleStyle {
    const char* name{}; // Unique; must outlive the registry (a string literal is typical).
    MaterialId material{};
    unsigned count{12};   // Particles per burst.
    float speed{1};       // Slowest launch speed, units per second.
    float speed_range{2}; // Launch speeds span [speed, speed + speed_range).
    float life{0.7F};     // Seconds.
    float size{0.13F};    // Starting size in world units.
    float drag{0.94F};    // Velocity multiplier per update.
    float spin{4};        // Radians per second.
    float shade{1.5F};    // Brightness multiplier.
};

// Transient cosmetic particles in a fixed pool. Games register styles once, then burst them.
class Particles final {
    struct Particle {
        WorldPosition position;
        Vec2 velocity;
        float life{}, angle{};
        ParticleStyleId style{};
    };

public:
    static constexpr std::size_t style_capacity = 32;
    ParticleStyleId add_style(const ParticleStyle& style) {
        if (!style.name || !*style.name) throw std::invalid_argument("A particle style needs a name");
        for (std::size_t i = 0; i < style_count_; ++i)
            if (std::string_view(styles_[i].name) == style.name)
                throw std::invalid_argument(std::string("Duplicate particle style: ") + style.name);
        if (!style.count || style.count > 512 || !(style.life > 0) || !(style.size > 0) || style.speed < 0 ||
            style.speed_range < 0 || style.drag < 0 || style.drag > 1)
            throw std::invalid_argument(std::string("Particle style out of range: ") + style.name);
        if (style_count_ == style_capacity) throw std::length_error("Too many particle styles");
        styles_[style_count_] = style;
        return static_cast<ParticleStyleId>(style_count_++);
    }
    ParticleStyleId find_style(std::string_view name) const {
        for (std::size_t i = 0; i < style_count_; ++i)
            if (styles_[i].name == name) return static_cast<ParticleStyleId>(i);
        throw std::out_of_range("Unknown particle style: " + std::string(name));
    }
    // Emits one burst; particles beyond the pool's 512 slots are dropped.
    void burst(WorldPosition origin, ParticleStyleId id) {
        if (id >= style_count_) throw std::out_of_range("Unregistered particle style");
        const auto& style = styles_[id];
        for (unsigned n = 0; n < style.count && count_ < ids_.size(); ++n) {
            state_ = mix64(state_);
            const float angle = static_cast<float>(state_ & 65535) * 6.2831853F / 65536;
            const float speed =
                style.speed + static_cast<float>((state_ >> 16) & 255) / 256 * style.speed_range;
            ids_[count_++] = pool_.create(
                Particle{origin, {std::cos(angle) * speed, std::sin(angle) * speed}, style.life, angle, id});
        }
    }
    void update(float dt) {
        for (std::size_t i = 0; i < count_;) {
            auto& p = pool_.get(ids_[i]);
            p.life -= dt;
            if (p.life <= 0) {
                pool_.destroy(ids_[i]);
                ids_[i] = ids_[--count_];
                continue;
            }
            const auto& style = styles_[p.style];
            p.position.move(p.velocity * dt);
            p.velocity = p.velocity * style.drag;
            p.angle += dt * style.spin;
            ++i;
        }
    }
    void draw(Renderer& renderer, WorldPosition camera) {
        for (std::size_t i = 0; i < count_; ++i) {
            const auto& p = pool_.get(ids_[i]);
            if (!nearby(p.position.chunk, camera.chunk, 3)) continue;
            const auto& style = styles_[p.style];
            const auto offset = relative(p.position, camera);
            const auto size = style.size * p.life / style.life;
            renderer.sprite(style.material, offset.x, offset.y, size, size, p.angle, style.shade);
        }
    }
    std::size_t count() const { return count_; }

private:
    Pool<Particle, 512> pool_;
    std::array<std::size_t, 512> ids_{};
    std::array<ParticleStyle, style_capacity> styles_{};
    std::size_t count_{}, style_count_{};
    std::uint64_t state_ = 1;
};
} // namespace seed
