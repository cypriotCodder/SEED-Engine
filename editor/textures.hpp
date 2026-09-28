#pragma once
#include "assets/pack_writer.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace seed::editor {
// A project's textures are image files in assets/textures (PNG, JPG or TGA), named by their file
// name without the extension. The editor compresses them into .seed/textures.pak, which the Scene
// view, Play and exported games use; games never decode images themselves.
std::filesystem::path texture_folder(const std::filesystem::path& project);
std::filesystem::path texture_pack(const std::filesystem::path& project);
std::vector<std::string> list_textures(const std::filesystem::path& project);

// Copies an image into the project's textures, under a name made from its file name (letters,
// digits, '_' and '-'; numbered if taken), and returns that name. Throws if it cannot be read.
std::string import_texture(const std::filesystem::path& project, const std::filesystem::path& image);

// Reads an image for packing: RGBA, bottom row first, sides rounded to multiples of four (at most
// 4096) by resampling.
PackImage read_texture(const std::filesystem::path& file, const std::string& name);

// Brings .seed/textures.pak up to date with the texture files: rebuilds it when any was added,
// removed or changed, and removes it when there are none. Returns true when the pack changed.
bool cook_textures(const std::filesystem::path& project);
} // namespace seed::editor
