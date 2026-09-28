#pragma once
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>

namespace seed {
// Reads one of a project's files by its path inside the project ("assets/materials.json",
// "scripts/player.lua"); nullopt when the file does not exist. A project is read the same way
// whether it is a folder being edited or an exported game's archive.
using ProjectFiles = std::function<std::optional<std::string>(const std::string& path)>;
ProjectFiles folder_files(std::filesystem::path root);
// The files under `prefix` ("assets/"), read with paths relative to it.
ProjectFiles subfolder(ProjectFiles files, std::string prefix);

// Whether `path` may name a file in a project: relative, '/'-separated, no "." or ".." parts, and
// only letters, digits, '_', '-' and '.'. Keeps archive contents from pointing anywhere else.
bool valid_project_path(const std::string& path);

// An exported game's data (game.seedpack): the project's files in one checksummed LZ4 envelope.
// Payload: u32 magic "SPAK", u32 version 1, u32 file count, then per file in path order a u16
// path length, the path, a u32 size and the bytes.
struct ProjectArchive {
    static constexpr const char* file_name = "game.seedpack";
    std::map<std::string, std::string> files;

    void write(const std::filesystem::path& file) const;
    // Throws on any malformed, unsafe, duplicate or trailing data.
    static ProjectArchive read(const std::filesystem::path& file);
    // Reads from a shared copy of the files, so the reader outlives this archive.
    ProjectFiles reader() const;
};
} // namespace seed
