#include "project/assets.hpp"
#include "assets/sound_file.hpp"
#include "io/storage.hpp"
#include <algorithm>
#include <charconv>

namespace seed {
namespace {
constexpr int assets_format = 1;
constexpr const char* kinds[] = {"materials", "actions", "sounds", "particles"};
constexpr const char* pattern_names[] = {"speckle", "water", "planks", "round"};
constexpr const char* mouse_names[] = {"Mouse Left", "Mouse Middle", "Mouse Right", "Mouse X1", "Mouse X2"};

float get_float(const Json& object, std::string_view key, float fallback) {
    const auto* value = object.find(key);
    return value ? static_cast<float>(value->as_number()) : fallback;
}
std::string get_string(const Json& object, std::string_view key) {
    const auto* value = object.find(key);
    return value ? value->as_string() : std::string();
}

// Registration of single entries. Names point into the asset, which must outlive the registry.
void add_material(Materials& out, const MaterialAsset& material) {
    out.add({.name = material.name.c_str(),
             .color = material.color,
             .pattern = material.pattern,
             .variation = material.variation,
             .texture = material.texture.empty() ? nullptr : material.texture.c_str(),
             .texture_scale = material.texture_scale,
             .frames = material.frames,
             .fps = material.fps,
             .speed = material.speed,
             .tags = material.tags.empty() ? nullptr : material.tags.c_str()});
}
void add_action(Actions& out, const ActionAsset& action) {
    out.add(action.name.c_str(), action.bindings);
}
void add_particle(const Materials& materials, Particles& out, const ParticleAsset& particle) {
    ParticleStyle style;
    style.name = particle.name.c_str();
    style.material = materials.find(particle.material);
    style.count = particle.count;
    style.speed = particle.speed;
    style.speed_range = particle.speed_range;
    style.life = particle.life;
    style.size = particle.size;
    style.drag = particle.drag;
    style.spin = particle.spin;
    style.shade = particle.shade;
    out.add_style(style);
}

// Adds every entry, collecting each failure instead of stopping at the first.
template<class List, class Add>
void check_all(std::string& problems, const char* kind, const List& list, Add&& add) {
    for (const auto& entry : list)
        try {
            add(entry);
        } catch (const std::exception& error) {
            problems += std::string(kind) + " \"" + entry.name + "\": " + error.what() + "\n";
        }
}

// Reads one kind's list, naming the entry when one is malformed.
template<class T, class F>
std::vector<T> read_list(const Json& file, const char* kind, F&& read) {
    if (file.at("format").as_int(1, 1000000) > assets_format)
        throw std::runtime_error("Made by a newer editor");
    std::vector<T> result;
    for (const auto& entry : file.at(kind).items())
        try {
            result.push_back(read(entry));
        } catch (const std::exception& error) {
            const auto* name = entry.is(Json::Type::object) ? entry.find("name") : nullptr;
            const bool named = name && name->is(Json::Type::string);
            throw std::runtime_error("Entry " + std::to_string(result.size() + 1) +
                                     (named ? " (\"" + name->as_string() + "\")" : std::string()) + ": " +
                                     error.what());
        }
    return result;
}

MaterialAsset read_material(const Json& entry) {
    MaterialAsset material;
    material.name = entry.at("name").as_string();
    const auto& color = entry.at("color").items();
    if (color.size() != 3) throw std::runtime_error("color needs three values");
    for (std::size_t i = 0; i < 3; ++i)
        material.color[i] = static_cast<int>(color[i].as_int(0, 255));
    if (const auto* pattern = entry.find("pattern")) {
        const auto& name = pattern->as_string();
        const auto* found = std::find(std::begin(pattern_names), std::end(pattern_names), name);
        if (found == std::end(pattern_names)) throw std::runtime_error("Unknown pattern \"" + name + "\"");
        material.pattern = static_cast<Pattern>(found - std::begin(pattern_names));
    }
    if (const auto* variation = entry.find("variation"))
        material.variation = static_cast<int>(variation->as_int(1, 255));
    material.texture = get_string(entry, "texture");
    material.texture_scale = get_float(entry, "texture_scale", 1);
    if (const auto* frames = entry.find("frames"))
        material.frames = static_cast<unsigned>(frames->as_int(1, 64));
    material.fps = get_float(entry, "fps", 0);
    material.speed = get_float(entry, "speed", 1);
    if (const auto* tags = entry.find("tags"))
        for (const auto& tag : tags->items())
            material.tags += (material.tags.empty() ? "" : " ") + tag.as_string();
    return material;
}
ActionAsset read_action(const Json& entry) {
    ActionAsset action;
    action.name = entry.at("name").as_string();
    if (const auto* bindings = entry.find("bindings"))
        for (const auto& binding : bindings->items())
            action.bindings.push_back(parse_binding(binding.as_string()));
    return action;
}
SoundAsset read_sound(const Json& entry) {
    SoundAsset sound, defaults;
    sound.name = entry.at("name").as_string();
    sound.frequency = get_float(entry, "frequency", defaults.frequency);
    sound.variation = get_float(entry, "variation", defaults.variation);
    sound.gain = get_float(entry, "gain", defaults.gain);
    sound.decay = get_float(entry, "decay", defaults.decay);
    sound.tone = get_float(entry, "tone", defaults.tone);
    sound.file = get_string(entry, "file");
    return sound;
}
ParticleAsset read_particle(const Json& entry) {
    ParticleAsset particle, defaults;
    particle.name = entry.at("name").as_string();
    particle.material = entry.at("material").as_string();
    if (const auto* count = entry.find("count"))
        particle.count = static_cast<unsigned>(count->as_int(1, 512));
    particle.speed = get_float(entry, "speed", defaults.speed);
    particle.speed_range = get_float(entry, "speed_range", defaults.speed_range);
    particle.life = get_float(entry, "life", defaults.life);
    particle.size = get_float(entry, "size", defaults.size);
    particle.drag = get_float(entry, "drag", defaults.drag);
    particle.spin = get_float(entry, "spin", defaults.spin);
    particle.shade = get_float(entry, "shade", defaults.shade);
    return particle;
}

// Each entry lists every field, defaults included, so a file is complete on its own.
Json write_material(const MaterialAsset& material) {
    auto entry = Json::object();
    entry.set("name", material.name);
    auto color = Json::array();
    for (const int c : material.color)
        color.push(c);
    entry.set("color", color);
    entry.set("pattern", pattern_names[static_cast<int>(material.pattern)]);
    entry.set("variation", material.variation);
    if (!material.texture.empty()) {
        entry.set("texture", material.texture);
        entry.set("texture_scale", json_float(material.texture_scale));
        if (material.frames > 1) {
            entry.set("frames", static_cast<int>(material.frames));
            entry.set("fps", json_float(material.fps));
        }
    }
    // Written only when set, so projects that never use them keep their files unchanged.
    if (material.speed != 1) entry.set("speed", json_float(material.speed));
    if (!material.tags.empty()) {
        auto tags = Json::array();
        for (std::size_t start = 0; start <= material.tags.size();) {
            auto end = material.tags.find(' ', start);
            if (end == std::string::npos) end = material.tags.size();
            tags.push(material.tags.substr(start, end - start));
            start = end + 1;
        }
        entry.set("tags", tags);
    }
    return entry;
}
Json write_action(const ActionAsset& action) {
    auto entry = Json::object();
    entry.set("name", action.name);
    auto bindings = Json::array();
    for (const auto& binding : action.bindings)
        bindings.push(binding_name(binding));
    entry.set("bindings", bindings);
    return entry;
}
Json write_sound(const SoundAsset& sound) {
    auto entry = Json::object();
    entry.set("name", sound.name);
    entry.set("frequency", json_float(sound.frequency));
    entry.set("variation", json_float(sound.variation));
    entry.set("gain", json_float(sound.gain));
    entry.set("decay", json_float(sound.decay));
    entry.set("tone", json_float(sound.tone));
    if (!sound.file.empty()) entry.set("file", sound.file);
    return entry;
}
Json write_particle(const ParticleAsset& particle) {
    auto entry = Json::object();
    entry.set("name", particle.name);
    entry.set("material", particle.material);
    entry.set("count", static_cast<int>(particle.count));
    entry.set("speed", json_float(particle.speed));
    entry.set("speed_range", json_float(particle.speed_range));
    entry.set("life", json_float(particle.life));
    entry.set("size", json_float(particle.size));
    entry.set("drag", json_float(particle.drag));
    entry.set("spin", json_float(particle.spin));
    entry.set("shade", json_float(particle.shade));
    return entry;
}

template<class List, class Write>
Json file_of(const char* kind, const List& list, Write&& write) {
    auto items = Json::array();
    for (const auto& entry : list)
        items.push(write(entry));
    auto file = Json::object();
    file.set("format", assets_format);
    file.set(kind, items);
    return file;
}
} // namespace

std::string binding_name(const Binding& binding) {
    if (binding.kind == Binding::Kind::mouse && binding.code >= 1 && binding.code <= 5)
        return mouse_names[binding.code - 1];
    if (binding.kind == Binding::Kind::key) {
        const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(binding.code));
        if (name && *name) return name;
    }
    throw std::invalid_argument("This input has no name");
}

Binding parse_binding(std::string_view name) {
    for (int i = 0; i < 5; ++i)
        if (name == mouse_names[i]) return Binding::mouse(i + 1);
    const std::string text(name);
    const auto scancode = SDL_GetScancodeFromName(text.c_str());
    if (scancode == SDL_SCANCODE_UNKNOWN) throw std::runtime_error("Unknown input \"" + text + "\"");
    return Binding::key(scancode);
}

void Assets::register_materials(Materials& out) const {
    for (const auto& material : materials)
        add_material(out, material);
}

void Assets::register_actions(Actions& out) const {
    for (const auto& action : actions)
        add_action(out, action);
}

void Assets::register_effects(const Materials& registry, Sounds& out_sounds, Particles& out_particles,
                              const std::vector<std::vector<float>>* samples) const {
    for (std::size_t i = 0; i < sounds.size(); ++i) {
        auto desc = sounds[i].desc();
        if (samples && i < samples->size() && !(*samples)[i].empty()) {
            desc.samples = (*samples)[i].data();
            desc.sample_count = static_cast<std::uint32_t>((*samples)[i].size());
        }
        out_sounds.add(desc);
    }
    for (const auto& particle : particles)
        add_particle(registry, out_particles, particle);
}

std::vector<std::string> Assets::material_names() const {
    std::vector<std::string> names;
    for (const auto& m : materials)
        names.push_back(m.name);
    return names;
}

std::string Assets::problems() const {
    std::string result;
    Materials material_registry;
    Actions action_registry;
    add_engine_actions(action_registry);
    Sounds sound_registry;
    Particles particle_registry;
    check_all(result, "Material", materials, [&](const auto& m) { add_material(material_registry, m); });
    check_all(result, "Action", actions, [&](const auto& a) { add_action(action_registry, a); });
    check_all(result, "Sound", sounds, [&](const auto& s) {
        if (!s.file.empty() && !valid_audio_file(s.file))
            throw std::invalid_argument(
                "Sound files are .wav or .ogg names of letters, digits, '_', '-' and '.'");
        sound_registry.add(s.desc());
    });
    check_all(result, "Particle style", particles,
              [&](const auto& p) { add_particle(material_registry, particle_registry, p); });
    const auto add_terrain_problems = [&](const std::string& label, const TerrainAsset& t) {
        const auto found = t.problems(material_names());
        for (std::size_t start = 0; start < found.size();) {
            const auto end = found.find('\n', start);
            result += label + found.substr(start, end - start + 1);
            start = end + 1;
        }
    };
    add_terrain_problems("Terrain: ", terrain);
    for (const auto& [name, t] : terrains) {
        if (!valid_terrain_name(name))
            result += "Terrain \"" + name + "\": names use letters, digits, '_' and '-'\n";
        add_terrain_problems("Terrain \"" + name + "\": ", t);
    }
    return result;
}

bool valid_audio_file(std::string_view name, bool music) {
    const auto ends = [&](std::string_view suffix) {
        return name.size() > suffix.size() && name.ends_with(suffix);
    };
    return name.size() <= 100 && (ends(".ogg") || (!music && ends(".wav"))) &&
           name.find("..") == std::string_view::npos && std::all_of(name.begin(), name.end(), [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '_' || c == '-' || c == '.';
           });
}

std::vector<std::vector<float>> load_sound_samples(const ProjectFiles& assets,
                                                   const std::vector<SoundAsset>& sounds) {
    std::vector<std::vector<float>> samples(sounds.size());
    for (std::size_t i = 0; i < sounds.size(); ++i) {
        if (sounds[i].file.empty()) continue;
        const auto where = "Sound \"" + sounds[i].name + "\": sounds/" + sounds[i].file;
        const auto bytes = assets ? assets("sounds/" + sounds[i].file) : std::nullopt;
        if (!bytes) throw std::runtime_error(where + " does not exist");
        try {
            samples[i] = decode_sound(*bytes);
        } catch (const std::exception& error) {
            throw std::runtime_error(where + ": " + error.what());
        }
    }
    return samples;
}

bool valid_terrain_name(std::string_view name) {
    return !name.empty() && name.size() <= 64 && std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
               c == '-';
    });
}

Json terrain_file_json(const TerrainAsset& terrain) {
    auto file = Json::object();
    file.set("format", assets_format);
    file.set("terrain", terrain_json(terrain));
    return file;
}

TerrainAsset parse_terrain_file(const Json& file) {
    if (file.at("format").as_int(1, 1000000) > assets_format)
        throw std::runtime_error("Made by a newer editor");
    return parse_terrain(file.at("terrain"));
}

Json assets_json(const Assets& assets, std::string_view kind) {
    if (kind == "materials") return file_of("materials", assets.materials, write_material);
    if (kind == "actions") return file_of("actions", assets.actions, write_action);
    if (kind == "sounds") return file_of("sounds", assets.sounds, write_sound);
    if (kind == "particles") return file_of("particles", assets.particles, write_particle);
    throw std::invalid_argument("Unknown asset kind");
}

TerrainAsset load_terrain(const ProjectFiles& files, const std::string& name) {
    if (!valid_terrain_name(name)) throw std::runtime_error("Invalid terrain name \"" + name + "\"");
    auto text = files("terrains/" + name + ".json");
    // Projects from before named terrains kept their one terrain in terrain.json.
    if (!text && name == "main") text = files("terrain.json");
    if (!text) return {};
    try {
        return parse_terrain_file(parse_json(*text));
    } catch (const std::exception& error) {
        throw std::runtime_error("terrains/" + name + ".json: " + error.what());
    }
}

Assets load_assets(const std::filesystem::path& folder) {
    auto assets = load_assets(folder_files(folder));
    // Every terrain in terrains/, or the old single terrain.json as "main".
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(folder / "terrains", error)) {
        const auto name = entry.path().stem().string();
        if (entry.path().extension() == ".json" && valid_terrain_name(name))
            assets.terrains[name] = load_terrain(folder_files(folder), name);
    }
    if (assets.terrains.empty() && std::filesystem::exists(folder / "terrain.json"))
        assets.terrains["main"] = load_terrain(folder_files(folder), "main");
    return assets;
}

Assets load_assets(const ProjectFiles& files) {
    Assets assets;
    for (const char* kind : kinds) {
        const auto name = std::string(kind) + ".json";
        const auto text = files(name);
        if (!text) continue;
        try {
            const auto file = parse_json(*text);
            const std::string_view k = kind;
            if (k == "materials") assets.materials = read_list<MaterialAsset>(file, kind, read_material);
            if (k == "actions") assets.actions = read_list<ActionAsset>(file, kind, read_action);
            if (k == "sounds") assets.sounds = read_list<SoundAsset>(file, kind, read_sound);
            if (k == "particles") assets.particles = read_list<ParticleAsset>(file, kind, read_particle);
        } catch (const std::exception& error) {
            throw std::runtime_error(name + ": " + error.what());
        }
    }
    return assets;
}

void save_assets(const std::filesystem::path& folder, const Assets& assets, const Assets* previous) {
    std::filesystem::create_directories(folder);
    for (const char* kind : kinds) {
        const auto file = assets_json(assets, kind);
        if (previous && assets_json(*previous, kind) == file) continue;
        write_text(folder / (std::string(kind) + ".json"), to_json(file));
    }
    // Terrains: changed ones rewritten, removed ones deleted. The old single terrain.json has moved
    // to terrains/main.json once terrains are saved.
    const auto terrains = folder / "terrains";
    for (const auto& [name, terrain] : assets.terrains) {
        if (previous && previous->terrains.count(name) && previous->terrains.at(name) == terrain &&
            std::filesystem::exists(terrains / (name + ".json")))
            continue;
        std::filesystem::create_directories(terrains);
        write_text(terrains / (name + ".json"), to_json(terrain_file_json(terrain)));
    }
    if (previous)
        for (const auto& [name, terrain] : previous->terrains)
            if (!assets.terrains.count(name)) std::filesystem::remove(terrains / (name + ".json"));
    if (!assets.terrains.empty()) std::filesystem::remove(folder / "terrain.json");
}
} // namespace seed
