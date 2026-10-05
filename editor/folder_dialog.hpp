#pragma once
#include <filesystem>
#include <optional>
#include <string>

namespace seed::editor {
// Whether this platform has a native folder picker. Where it does not, the editor offers only a
// typed path.
bool folder_dialog_available();
// Shows the native folder picker and blocks until the user chooses a folder or cancels.
std::optional<std::filesystem::path> choose_folder(const char* title, const std::filesystem::path& start);
// Shows the native file picker for an image (PNG, JPG or TGA) and blocks until one is chosen.
std::optional<std::filesystem::path> choose_image(const char* title);
// The same for a sound (WAV or Ogg Vorbis), or for music (Ogg Vorbis only).
std::optional<std::filesystem::path> choose_audio(const char* title, bool music);
} // namespace seed::editor
