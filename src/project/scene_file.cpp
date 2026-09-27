#include "project/scene_file.hpp"
#include "io/storage.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>

namespace seed {
namespace {
constexpr int scene_format = 1;

Json pair(float a, float b) {
    auto list = Json::array();
    list.push(json_float(a));
    list.push(json_float(b));
    return list;
}
std::array<float, 2> read_pair(const Json& json) {
    const auto& items = json.items();
    if (items.size() != 2) throw std::runtime_error("Expected two numbers");
    return {static_cast<float>(items[0].as_number()), static_cast<float>(items[1].as_number())};
}
bool finite(float value) {
    return std::isfinite(value);
}
} // namespace

std::string SceneFile::problems(const Assets& assets) const {
    std::string result;
    const auto report = [&](std::size_t i, const std::string& text) {
        result += "Entity \"" + entities[i].name + "\" (" + std::to_string(i + 1) + "): " + text + "\n";
    };
    if (entities.size() > capacity) result += "More than 4096 entities in one scene\n";
    for (std::size_t i = 0; i < entities.size(); ++i) {
        const auto& e = entities[i];
        if (e.name.empty() || e.name.size() > 64) report(i, "Names need 1 to 64 characters");
        if (!finite(e.position.local.x) || !finite(e.position.local.y) || !finite(e.angle))
            report(i, "Position and angle must be finite");
        if (e.visual) {
            const auto& v = *e.visual;
            if (std::none_of(assets.materials.begin(), assets.materials.end(),
                             [&](const MaterialAsset& m) { return m.name == v.material; }))
                report(i, "Unknown material \"" + v.material + "\"");
            if (!(v.size.x > 0 && v.size.x <= 64 && v.size.y > 0 && v.size.y <= 64))
                report(i, "Visual size must be above 0 and at most 64");
        }
        if (e.light) {
            const auto& l = *e.light;
            const bool color =
                std::all_of(l.color.begin(), l.color.end(), [](float c) { return c >= 0 && c <= 16; });
            if (!color || !(l.radius > 0 && l.radius <= 256) || !(l.intensity >= 0 && l.intensity <= 64) ||
                !(l.height > 0 && l.height <= 64))
                report(i, "Light values out of range");
        }
    }
    return result;
}

Json scene_json(const SceneFile& scene) {
    auto entities = Json::array();
    for (const auto& e : scene.entities) {
        auto entity = Json::object();
        entity.set("name", e.name);
        auto chunk = Json::array();
        chunk.push(e.position.chunk.x);
        chunk.push(e.position.chunk.y);
        entity.set("chunk", chunk);
        entity.set("position", pair(e.position.local.x, e.position.local.y));
        entity.set("angle", json_float(e.angle));
        if (e.visual) {
            auto visual = Json::object();
            visual.set("material", e.visual->material);
            visual.set("size", pair(e.visual->size.x, e.visual->size.y));
            entity.set("visual", visual);
        }
        if (e.light) {
            auto light = Json::object();
            auto color = Json::array();
            for (const float c : e.light->color)
                color.push(json_float(c));
            light.set("color", color);
            light.set("radius", json_float(e.light->radius));
            light.set("intensity", json_float(e.light->intensity));
            light.set("height", json_float(e.light->height));
            entity.set("light", light);
        }
        entities.push(entity);
    }
    auto file = Json::object();
    file.set("format", scene_format);
    file.set("entities", entities);
    return file;
}

SceneFile parse_scene(const Json& json) {
    if (json.at("format").as_int(1, 1000000) > scene_format)
        throw std::runtime_error("Made by a newer editor");
    SceneFile scene;
    const auto& list = json.at("entities").items();
    if (list.size() > SceneFile::capacity) throw std::runtime_error("More than 4096 entities");
    for (const auto& item : list)
        try {
            SceneEntity e;
            e.name = item.at("name").as_string();
            const auto& chunk = item.at("chunk").items();
            if (chunk.size() != 2) throw std::runtime_error("chunk needs two whole numbers");
            // Chunk coordinates stay within the exactly representable range of a double.
            e.position.chunk = {chunk[0].as_int(), chunk[1].as_int()};
            const auto local = read_pair(item.at("position"));
            e.position.local = {local[0], local[1]};
            e.position.move({}); // Canonicalizes offsets outside [0, chunk_side).
            if (const auto* angle = item.find("angle")) e.angle = static_cast<float>(angle->as_number());
            if (const auto* visual = item.find("visual")) {
                SceneVisual v;
                v.material = visual->at("material").as_string();
                const auto size = read_pair(visual->at("size"));
                v.size = {size[0], size[1]};
                e.visual = v;
            }
            if (const auto* light = item.find("light")) {
                SceneLight l;
                const auto& color = light->at("color").items();
                if (color.size() != 3) throw std::runtime_error("color needs three numbers");
                for (std::size_t c = 0; c < 3; ++c)
                    l.color[c] = static_cast<float>(color[c].as_number());
                l.radius = static_cast<float>(light->at("radius").as_number());
                l.intensity = static_cast<float>(light->at("intensity").as_number());
                l.height = static_cast<float>(light->at("height").as_number());
                e.light = l;
            }
            scene.entities.push_back(std::move(e));
        } catch (const std::exception& error) {
            throw std::runtime_error("Entity " + std::to_string(scene.entities.size() + 1) + ": " +
                                     error.what());
        }
    return scene;
}

SceneFile load_scene(const std::filesystem::path& file) {
    try {
        return parse_scene(parse_json(read_text(file, 16 * 1024 * 1024)));
    } catch (const std::exception& error) {
        throw std::runtime_error(file.string() + ": " + error.what());
    }
}

void save_scene(const std::filesystem::path& file, const SceneFile& scene) {
    std::filesystem::create_directories(file.parent_path());
    write_text(file, to_json(scene_json(scene)));
}
} // namespace seed
