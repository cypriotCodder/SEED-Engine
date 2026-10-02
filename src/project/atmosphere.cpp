#include "project/atmosphere.hpp"
#include <algorithm>
#include <cmath>

namespace seed {
namespace {
constexpr SceneAtmosphere defaults{};

bool color_ok(const std::array<float, 3>& c) {
    return std::all_of(c.begin(), c.end(), [](float v) { return v >= 0 && v <= 4; });
}
Json color_json(const std::array<float, 3>& c) {
    auto list = Json::array();
    for (const float v : c)
        list.push(json_float(v));
    return list;
}
std::array<float, 3> read_color(const Json& object, const char* key, std::array<float, 3> fallback) {
    const auto* value = object.find(key);
    if (!value) return fallback;
    const auto& items = value->items();
    if (items.size() != 3) throw std::runtime_error(std::string(key) + " needs three numbers");
    for (std::size_t i = 0; i < 3; ++i)
        fallback[i] = static_cast<float>(items[i].as_number());
    return fallback;
}
float read(const Json& object, const char* key, float fallback) {
    const auto* value = object.find(key);
    return value ? static_cast<float>(value->as_number()) : fallback;
}
std::array<float, 3> mix(const std::array<float, 3>& a, const std::array<float, 3>& b, float t) {
    return {std::lerp(a[0], b[0], t), std::lerp(a[1], b[1], t), std::lerp(a[2], b[2], t)};
}
} // namespace

std::string SceneAtmosphere::problems() const {
    std::string out;
    if (!color_ok(ambient) || !color_ok(night) || !color_ok(background) || !color_ok(haze))
        out += "Atmosphere colours must be from 0 to 4\n";
    if (!(haze_amount >= 0 && haze_amount <= 1)) out += "Atmosphere haze must be from 0 to 1\n";
    if (!(hour >= 0 && hour <= 24)) out += "Atmosphere hour must be from 0 to 24\n";
    if (!(day_length == 0 || (day_length >= 10 && day_length <= 86400)))
        out += "A day lasts 10 to 86,400 seconds, or 0 for no day and night\n";
    return out;
}

// Only the settings that differ from the defaults are written, so most scenes have none.
Json atmosphere_json(const SceneAtmosphere& a) {
    auto out = Json::object();
    if (a.ambient != defaults.ambient) out.set("ambient", color_json(a.ambient));
    if (a.night != defaults.night) out.set("night", color_json(a.night));
    if (a.background != defaults.background) out.set("background", color_json(a.background));
    if (a.haze != defaults.haze) out.set("haze", color_json(a.haze));
    if (a.haze_amount != defaults.haze_amount) out.set("haze_amount", json_float(a.haze_amount));
    if (a.hour != defaults.hour) out.set("hour", json_float(a.hour));
    if (a.day_length != defaults.day_length) out.set("day_length", json_float(a.day_length));
    return out;
}

SceneAtmosphere parse_atmosphere(const Json& json) {
    SceneAtmosphere a;
    a.ambient = read_color(json, "ambient", a.ambient);
    a.night = read_color(json, "night", a.night);
    a.background = read_color(json, "background", a.background);
    a.haze = read_color(json, "haze", a.haze);
    a.haze_amount = read(json, "haze_amount", a.haze_amount);
    a.hour = read(json, "hour", a.hour);
    a.day_length = read(json, "day_length", a.day_length);
    return a;
}

float daylight(float hour) {
    // Distance from noon in hours, 0 to 12; full night beyond 9 hours from noon.
    const float from_noon = std::abs(std::fmod(std::fmod(hour, 24.0F) + 24.0F, 24.0F) - 12);
    const float t = std::clamp((9 - from_noon) / 6, 0.0F, 1.0F); // Full from 09:00 to 15:00.
    return t * t * (3 - 2 * t);
}

Lighting lighting_at(const SceneAtmosphere& a, float hour) {
    const float day = daylight(hour);
    Lighting out;
    out.ambient = mix(a.night, a.ambient, day);
    // The sky behind and the air darken towards night, but never to black.
    const float dim = std::lerp(0.35F, 1.0F, day);
    for (std::size_t i = 0; i < 3; ++i) {
        out.clear[i] = a.background[i] * dim;
        out.haze[i] = a.haze[i] * dim;
    }
    out.haze_amount = a.haze_amount;
    return out;
}

float light_intensity(const LightComponent& light, float day, double time, unsigned seed) {
    float intensity = light.intensity;
    if (light.night_only) intensity *= 1 - day;
    if (light.flicker > 0) {
        // Two sines at unrelated rates read as an irregular flame without any per-frame state.
        const auto t = static_cast<float>(std::fmod(time, 3600.0));
        const float phase = static_cast<float>(seed % 1024) * 2.399F;
        const float wave = 0.6F * std::sin(t * 7.3F + phase) + 0.4F * std::sin(t * 13.7F + phase * 2.3F);
        intensity *= 1 - light.flicker * 0.5F * (1 + wave) * 0.6F;
    }
    return intensity;
}

void DayClock::advance(double seconds) {
    if (atmosphere.day_length > 0) set_hour(hour + seconds / atmosphere.day_length * 24);
}

void DayClock::set_hour(double value) {
    hour = std::fmod(std::fmod(value, 24.0) + 24.0, 24.0);
}
} // namespace seed
