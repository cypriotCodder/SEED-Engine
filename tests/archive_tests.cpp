#include "io/binary.hpp"
#include "io/storage.hpp"
#include "project/archive.hpp"
#include "project/assets.hpp"
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

void paths() {
    for (const char* good :
         {"project.seed.json", "assets/materials.json", "scripts/player_2.lua", "a/b-c/d.e"})
        check(seed::valid_project_path(good), good);
    for (const char* bad :
         {"", "/etc/passwd", "../x", "a/../b", "a//b", "a/", "./a", "has space.lua", "a\\\\b", "é.lua"})
        check(!seed::valid_project_path(bad), bad);
}

void round_trip(const fs::path& root) {
    seed::ProjectArchive archive;
    archive.files["project.seed.json"] = R"({"seed_project":1})";
    archive.files["scripts/player.lua"] = "function update(dt) end\n";
    archive.files["assets/empty.json"] = "";
    archive.write(root / "game.seedpack");
    const auto back = seed::ProjectArchive::read(root / "game.seedpack");
    check(back.files == archive.files, "Archive reads back unchanged");
    const auto read = back.reader();
    check(read("scripts/player.lua") == archive.files["scripts/player.lua"] && !read("missing.lua"),
          "Reader");
    const auto scripts = seed::subfolder(read, "scripts/");
    check(scripts("player.lua").has_value(), "Subfolder reader");

    seed::ProjectArchive unsafe;
    unsafe.files["../escape"] = "x";
    rejects([&] { unsafe.write(root / "unsafe.seedpack"); }, "Unsafe path written");
}

// Hand-built payloads: every structural rule is enforced on read.
void malformed(const fs::path& root) {
    const auto build = [&](const std::vector<std::pair<std::string, std::string>>& files, bool trailing) {
        seed::Bytes out;
        out.u32(0x4b415053);
        out.u32(1);
        out.u32(static_cast<std::uint32_t>(files.size()));
        for (const auto& [path, bytes] : files) {
            out.u16(static_cast<std::uint16_t>(path.size()));
            out.data.insert(out.data.end(), path.begin(), path.end());
            out.u32(static_cast<std::uint32_t>(bytes.size()));
            out.data.insert(out.data.end(), bytes.begin(), bytes.end());
        }
        if (trailing) out.u8(0);
        seed::write_blob(root / "bad.seedpack", out.data);
        return root / "bad.seedpack";
    };
    rejects([&] { seed::ProjectArchive::read(build({{"b", ""}, {"a", ""}}, false)); },
            "Out-of-order files accepted");
    rejects([&] { seed::ProjectArchive::read(build({{"a", ""}, {"a", ""}}, false)); },
            "Duplicate files accepted");
    rejects([&] { seed::ProjectArchive::read(build({{"../a", ""}}, false)); }, "Unsafe path accepted");
    rejects([&] { seed::ProjectArchive::read(build({{"a", ""}}, true)); }, "Trailing data accepted");
    seed::ProjectArchive::read(build({{"a", "x"}, {"b", ""}}, false));

    // A flipped byte fails the envelope's checksum.
    auto bytes = seed::read_text(root / "game.seedpack");
    bytes[bytes.size() - 1] ^= 1;
    seed::write_text(root / "tampered.seedpack", bytes);
    rejects([&] { seed::ProjectArchive::read(root / "tampered.seedpack"); }, "Corrupted archive accepted");
}

// Assets load the same from a folder and from an archive of it.
void assets_from_archive(const fs::path& root) {
    seed::Assets assets;
    seed::MaterialAsset grass;
    grass.name = "grass";
    assets.materials = {grass};
    seed::save_assets(root / "assets", assets);
    seed::ProjectArchive archive;
    archive.files["assets/materials.json"] =
        seed::to_json(seed::parse_json(seed::read_text(root / "assets" / "materials.json")), true);
    check(archive.files["assets/materials.json"].find(' ') == std::string::npos,
          "Compact JSON has no spaces");
    check(seed::load_assets(seed::subfolder(archive.reader(), "assets/")) ==
              seed::load_assets(root / "assets"),
          "Archive and folder load the same assets");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::invalid_argument("Usage: seed_archive_tests SCRATCH_DIRECTORY");
        const fs::path root = argv[1];
        fs::remove_all(root);
        fs::create_directories(root);
        paths();
        round_trip(root);
        malformed(root);
        assets_from_archive(root);
        std::cout << "Project archive checks passed.\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
