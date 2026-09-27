#include "project/scene_file.hpp"
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

seed::Assets assets() {
    seed::Assets result;
    seed::MaterialAsset grass;
    grass.name = "grass";
    result.materials = {grass};
    return result;
}

seed::SceneFile sample() {
    seed::SceneFile scene;
    seed::SceneEntity ground;
    ground.name = "Ground";
    ground.position = {{-3, 1000000}, {31.5F, 0.25F}};
    ground.angle = 0.5F;
    ground.visual = seed::SceneVisual{"grass", {4, 2}};
    seed::SceneEntity lamp;
    lamp.name = "Lamp";
    lamp.light = seed::SceneLight{};
    seed::SceneEntity spawn;
    spawn.name = "Spawn";
    scene.entities = {ground, lamp, spawn};
    return scene;
}

void round_trip(const fs::path& root) {
    const auto scene = sample();
    check(scene.problems(assets()).empty(), "Sample scene is valid");
    seed::save_scene(root / "scenes" / "main.json", scene);
    const auto loaded = seed::load_scene(root / "scenes" / "main.json");
    check(loaded == scene, "Scene reads back unchanged, including far chunk coordinates");
    check(!loaded.entities[2].visual && !loaded.entities[2].light, "Component-free entities stay empty");
}

void canonical() {
    // Offsets outside [0, 32) move into the neighbouring chunk.
    const auto scene = seed::parse_scene(seed::parse_json(
        R"({"format": 1, "entities": [{"name": "A", "chunk": [0, 0], "position": [-1, 40], "angle": 0}]})"));
    const auto& p = scene.entities[0].position;
    check(p.chunk == seed::ChunkCoord{-1, 1} && p.local.x == 31 && p.local.y == 8, "Positions canonicalized");
}

void problems() {
    auto scene = sample();
    scene.entities[0].visual->material = "stone";
    scene.entities[1].light->radius = 0;
    scene.entities[2].name.clear();
    const auto report = scene.problems(assets());
    check(report.find("Unknown material \"stone\"") != std::string::npos, "Unknown material reported");
    check(report.find("Light values out of range") != std::string::npos, "Bad light reported");
    check(report.find("Names need") != std::string::npos, "Empty name reported");
    check(std::count(report.begin(), report.end(), '\n') == 3, "One line per problem");
}

void malformed() {
    const char* bad[] = {
        R"({"format": 2, "entities": []})",
        R"({"format": 1})",
        R"({"format": 1, "entities": [{"name": "A", "chunk": [0], "position": [0, 0]}]})",
        R"({"format": 1, "entities": [{"name": "A", "chunk": [0.5, 0], "position": [0, 0]}]})",
        R"({"format": 1, "entities": [{"name": "A", "chunk": [0, 0], "position": [0, 0], "visual": {"size": [1, 1]}}]})",
        R"({"format": 1, "entities": [{"name": "A", "chunk": [0, 0], "position": [1e30, 0]}]})",
    };
    for (const char* text : bad)
        rejects([&] { seed::parse_scene(seed::parse_json(text)); }, text);
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::invalid_argument("Usage: seed_scene_tests SCRATCH_DIRECTORY");
        fs::remove_all(argv[1]);
        round_trip(argv[1]);
        canonical();
        problems();
        malformed();
        std::cout << "Scene file checks passed.\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
