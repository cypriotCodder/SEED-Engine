#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace seed::editor {
// A project's recorded sounds are WAV or Ogg Vorbis files in assets/sounds; its music is Ogg
// Vorbis files in assets/music, named by file name without the extension.
std::filesystem::path audio_folder(const std::filesystem::path& project, bool music);
// File names, in order.
std::vector<std::string> list_audio(const std::filesystem::path& project, bool music);
// Copies a sound or music file into the project, under a name made from its file name (letters,
// digits, '_' and '-'; numbered if taken), and returns that file name. It is decoded first, so a
// file the game could not play is refused with the reason.
std::string import_audio(const std::filesystem::path& project, const std::filesystem::path& source,
                         bool music);
} // namespace seed::editor
