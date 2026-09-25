#pragma once
#include "core/memory.hpp"
#include "render/renderer.hpp"
#include "world/noise.hpp"

namespace seed {
class Particles final {
    struct Particle {
        WorldPosition position;
        Vec2 velocity;
        float life{}, angle{};
    };

public:
    void burst(WorldPosition origin) {
        for (unsigned n = 0; n < 12 && count_ < ids_.size(); ++n) {
            state_ = mix64(state_);
            const float angle = static_cast<float>(state_ & 65535) * 6.2831853F / 65536;
            const float speed = 1 + static_cast<float>((state_ >> 16) & 255) / 128;
            ids_[count_++] = pool_.create(
                Particle{origin, {std::cos(angle) * speed, std::sin(angle) * speed}, 0.7F, angle});
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
            p.position.move(p.velocity * dt);
            p.velocity = p.velocity * 0.94F;
            p.angle += dt * 4;
            ++i;
        }
    }
    void draw(Renderer& renderer, WorldPosition camera, MaterialId material) {
        for (std::size_t i = 0; i < count_; ++i) {
            const auto& p = pool_.get(ids_[i]);
            if (!nearby(p.position.chunk, camera.chunk, 3)) continue;
            const auto offset = relative(p.position, camera);
            const auto size = 0.13F * p.life / 0.7F;
            renderer.sprite(material, offset.x, offset.y, size, size, p.angle, 1.5F);
        }
    }

private:
    Pool<Particle, 512> pool_;
    std::array<std::size_t, 512> ids_{};
    std::size_t count_{};
    std::uint64_t state_ = 1;
};
} // namespace seed
