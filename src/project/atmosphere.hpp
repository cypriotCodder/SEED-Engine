#pragma once
#include "io/json.hpp"
#include "render/renderer.hpp"
#include <array>
#include <string>

namespace seed {
// A scene's light and air: the light present everywhere, the background, the haze towards the
// view's edges, and an optional day that turns into night and back. The defaults are the
// renderer's own lighting at noon, so a scene that never changes them looks as it always did.
struct SceneAtmosphere {
    std::array<float, 3> ambient{0.34F, 0.43F, 0.56F};       // Light everywhere at noon.
    std::array<float, 3> night{0.06F, 0.08F, 0.16F};         // Light everywhere at midnight.
    std::array<float, 3> background{0.035F, 0.065F, 0.085F}; // Where nothing is drawn.
    std::array<float, 3> haze{0.085F, 0.13F, 0.17F};
    float haze_amount{0.2F}; // 0 to 1; 0 turns the haze off.
    float hour{12};          // Time of day the scene starts at, 0 to 24.
    float day_length{};      // Real seconds for a whole day; 0 keeps the time at `hour`.
    bool operator==(const SceneAtmosphere&) const = default;
    std::string problems() const; // One per line; empty when valid.
};
Json atmosphere_json(const SceneAtmosphere& atmosphere);
SceneAtmosphere parse_atmosphere(const Json& json);

// How bright the day is at an hour: 1 from 09:00 to 15:00, 0 from 21:00 to 03:00, easing between.
float daylight(float hour);
// The renderer's lighting for the atmosphere at an hour. At full daylight it is exactly the
// atmosphere's colours; towards night the ambient light moves to `night` and the background and
// haze darken with it.
Lighting lighting_at(const SceneAtmosphere& atmosphere, float hour);

// A light standing on an entity while a game runs.
struct LightComponent {
    std::array<float, 3> color{};
    float radius{}, intensity{}, height{};
    float flicker{};   // 0 to 1: how much the brightness wavers, as a flame's does.
    bool night_only{}; // Lit only as daylight fades, like a street lamp.
};
// The light's brightness this frame, at `time` seconds into the game. `seed` (such as the
// entity's index) keeps neighbouring flames from wavering in step.
float light_intensity(const LightComponent& light, float daylight, double time, unsigned seed = 0);

// The game's one clock. Until the game reaches a scene with a day, each scene starts at its own
// hour and the clock stands still. From the first scene with a day, the clock runs at the day
// length of the latest such scene, in every scene: a scene without a day keeps showing its own
// hour, and the day has moved on when the player comes out.
struct DayClock {
    SceneAtmosphere atmosphere; // The scene playing.
    double hour{12};            // The game's hour, 0 to 24.
    float day_length{};         // Real seconds per day; 0 until the game reaches a scene with a day.
    void enter(const SceneAtmosphere& scene); // On starting or changing scene.
    void advance(double seconds);
    void set_hour(double value);
    double shown() const; // The hour the scene playing is lit at.
};
} // namespace seed
