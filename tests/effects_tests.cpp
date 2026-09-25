#include "core/particles.hpp"
#include "platform/audio.hpp"
#include <iostream>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Exception, class F>
void throws(F&& f, const char* message) {
    try {
        f();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(message);
}
} // namespace

int main() {
    try {
        seed::Sounds sounds;
        const auto knock = sounds.add({"knock", 140, 180});
        check(knock == 0 && sounds[knock].frequency == 140, "Sound registration");
        throws<std::invalid_argument>([&] { sounds.add({"knock"}); }, "Duplicate sound accepted");
        throws<std::invalid_argument>([&] { sounds.add({"silent", 0}); }, "Zero frequency accepted");
        throws<std::invalid_argument>([&] { sounds.add({"forever", 140, 0, 0.2F, 1.0F}); },
                                      "Non-decaying sound accepted");
        // Audio that is switched off still validates IDs, so a bad ID fails in tests too.
        seed::Audio silent(false, sounds);
        silent.play(knock);
        throws<std::out_of_range>([&] { silent.play(5); }, "Unregistered sound played");

        seed::Particles particles;
        seed::ParticleStyle sparks;
        sparks.count = 5;
        sparks.life = 0.5F;
        const auto id = particles.add_style(sparks);
        throws<std::invalid_argument>([&] { particles.add_style({0, 0}); }, "Empty style accepted");
        throws<std::out_of_range>([&] { particles.burst({}, 9); }, "Unregistered style accepted");
        particles.burst({}, id);
        particles.burst({}, id);
        check(particles.count() == 10, "Each burst emits its style's count");
        particles.update(0.3F);
        check(particles.count() == 10, "Particles alive before their life ends");
        particles.update(0.3F);
        check(particles.count() == 0, "Particles expire after their life");
        seed::ParticleStyle flood;
        flood.count = 400;
        const auto big = particles.add_style(flood);
        particles.burst({}, big);
        particles.burst({}, big);
        check(particles.count() == 512, "Bursts stop at the pool size");
        std::cout << "Sound and particle style checks passed.\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
