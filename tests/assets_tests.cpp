#include "io/storage.hpp"
#include "project/assets.hpp"
#include <algorithm>
#include <filesystem>
#include <iostream>

namespace {
namespace fs = std::filesystem;
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F>
void rejects(F&& f, const char* message) {
    try {
        f();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

seed::MaterialAsset material(const char* name, std::array<int, 3> color) {
    seed::MaterialAsset result;
    result.name = name;
    result.color = color;
    return result;
}

seed::Assets sample() {
    seed::Assets assets;
    assets.materials = {material("grass", {90, 140, 70}), material("water", {30, 80, 120}),
                        material("crate", {150, 110, 60})};
    assets.materials[1].pattern = seed::Pattern::water;
    assets.materials[1].variation = 12;
    assets.materials[1].speed = 0.5F;
    assets.materials[1].tags = "wet slow";
    assets.materials[1].blend = 7;
    assets.materials[2].pattern = seed::Pattern::planks;
    assets.materials[2].texture = "crate.bc3";
    assets.materials[2].frames = 4;
    assets.materials[2].fps = 12;
    assets.actions = {{"jump", {seed::Binding::key(SDL_SCANCODE_SPACE)}},
                      {"run", {seed::Binding::key(SDL_SCANCODE_LSHIFT), seed::Binding::mouse(SDL_BUTTON_X1)}},
                      {"idle", {}}};
    assets.sounds = {{"knock", 140, 180, 0.2F, 0.9991F, 0.75F, "knock.ogg"}};
    seed::ParticleAsset splash;
    splash.name = "splash";
    splash.material = "water";
    splash.count = 20;
    assets.particles = {splash};
    return assets;
}

void round_trip(const fs::path& root) {
    const auto assets = sample();
    check(assets.problems().empty(), "Sample assets are valid");
    seed::save_assets(root, assets);
    check(seed::load_assets(root) == assets, "Assets read back unchanged");
    const auto sounds = seed::read_text(root / "sounds.json");
    check(sounds.find("\"decay\": 0.9991,") != std::string::npos, "Floats written in their shortest form");
    const auto actions = seed::read_text(root / "actions.json");
    check(actions.find("\"Left Shift\"") != std::string::npos &&
              actions.find("\"Mouse X1\"") != std::string::npos,
          "Bindings written by name");

    // Only changed kinds are rewritten.
    fs::remove(root / "materials.json");
    auto changed = assets;
    changed.sounds[0].gain = 0.5F;
    seed::save_assets(root, changed, &assets);
    check(!fs::exists(root / "materials.json"), "Unchanged kinds are not rewritten");
    check(seed::load_assets(root).sounds[0].gain == 0.5F, "Changed kind rewritten");

    check(seed::load_assets(root / "missing").materials.empty(), "A missing folder is an empty project");
}

void registration() {
    const auto assets = sample();
    seed::Materials materials;
    assets.register_materials(materials);
    check(materials.size() == 3 && materials.find("crate") == 2, "Materials registered in file order");
    check(std::string_view(materials[2].texture) == "crate.bc3" && !materials[0].texture,
          "Texture names kept");
    check(materials[1].speed == 0.5F && materials[0].speed == 1, "Ground speeds kept");
    check(materials[1].blend == 7 && materials[0].blend == 0, "Edge blends kept");
    check(materials[2].frames == 4 && materials[2].fps == 12 && materials[0].frames == 1, "Animation kept");
    check(materials.tagged(1, "wet") && materials.tagged(1, "slow") && !materials.tagged(1, "we") &&
              !materials.tagged(0, "wet"),
          "Tags are whole words");
    seed::Actions actions;
    seed::add_engine_actions(actions);
    assets.register_actions(actions);
    check(actions.find("run") == 4, "Project actions follow the engine's");
    seed::Sounds sounds;
    seed::Particles particles;
    assets.register_effects(materials, sounds, particles);
    check(sounds.size() == 1 && particles.find_style("splash") == 0, "Effects registered");

    check(seed::parse_binding(seed::binding_name(seed::Binding::key(SDL_SCANCODE_KP_ENTER))) ==
              seed::Binding::key(SDL_SCANCODE_KP_ENTER),
          "Key names round trip");
    rejects([] { seed::parse_binding("Not A Key"); }, "Unknown key accepted");
}

void problems() {
    auto assets = sample();
    assets.materials.push_back(material("grass", {1, 2, 3})); // Duplicate name.
    assets.materials.push_back(material("hot", {300, 0, 0})); // Colour out of range.
    assets.materials.push_back(material("ice", {200, 220, 255}));
    assets.materials.back().speed = 9; // Above 4.
    assets.materials.push_back(material("lava", {255, 80, 0}));
    assets.materials.back().tags = "hot  burns"; // Two spaces.
    assets.materials.push_back(material("goo", {0, 255, 0}));
    assets.materials.back().tags = "sticky!";
    assets.materials.push_back(material("reel", {9, 9, 9}));
    assets.materials.back().frames = 65; // Above 64.
    assets.materials.push_back(material("mist", {9, 9, 9}));
    assets.materials.back().blend = 256;    // Above 255.
    assets.actions.push_back({"quit", {}}); // Clashes with an engine action.
    seed::SoundAsset loud;
    loud.name = "loud";
    loud.gain = 2; // Above 1.
    assets.sounds.push_back(loud);
    seed::ParticleAsset dust;
    dust.name = "dust";
    dust.material = "sand"; // Unknown material.
    assets.particles.push_back(dust);
    const auto report = assets.problems();
    for (const char* expected :
         {"Material \"grass\"", "Material \"hot\"", "Material \"ice\"", "Material \"lava\"",
          "Material \"goo\"", "Material \"reel\"", "Material \"mist\"", "Action \"quit\"", "Sound \"loud\"",
          "Particle style \"dust\""})
        check(report.find(expected) != std::string::npos, expected);
    check(std::count(report.begin(), report.end(), '\n') == 10, "One line per problem");
}

void bad_files(const fs::path& root) {
    const auto write = [&](const char* file, const char* text) {
        fs::remove_all(root);
        fs::create_directories(root);
        seed::write_text(root / file, text);
    };
    write("materials.json", R"({"format": 1, "materials": [{"name": "a", "color": [1, 2]}]})");
    rejects([&] { seed::load_assets(root); }, "Short colour accepted");
    write("materials.json",
          R"({"format": 1, "materials": [{"name": "a", "color": [1, 2, 3], "blend": -1}]})");
    rejects([&] { seed::load_assets(root); }, "Negative blend accepted");
    write("materials.json",
          R"({"format": 1, "materials": [{"name": "a", "color": [1, 2, 3], "pattern": "zigzag"}]})");
    try {
        seed::load_assets(root);
        throw std::logic_error("Unknown pattern accepted");
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        check(message.find("materials.json") != std::string::npos &&
                  message.find("Entry 1 (\"a\")") != std::string::npos,
              "Errors name the file and the entry");
    }
    write("actions.json", R"({"format": 2, "actions": []})");
    rejects([&] { seed::load_assets(root); }, "Newer format accepted");
    write("sounds.json", R"({"format": 1, "sounds": [{"name": "s", "gain": "loud"}]})");
    rejects([&] { seed::load_assets(root); }, "Wrongly typed field accepted");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::invalid_argument("Usage: seed_assets_tests SCRATCH_DIRECTORY");
        const fs::path root = argv[1];
        fs::remove_all(root);
        round_trip(root / "project");
        registration();
        problems();
        bad_files(root / "bad");
        std::cout << "Project asset checks passed.\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
