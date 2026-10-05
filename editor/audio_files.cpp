#include "audio_files.hpp"
#include "assets/sound_file.hpp"
#include "io/storage.hpp"
#include "project/assets.hpp"
#include <algorithm>
#include <cctype>

namespace seed::editor {
namespace fs = std::filesystem;

fs::path audio_folder(const fs::path& project, bool music) {
    return project / "assets" / (music ? "music" : "sounds");
}

std::vector<std::string> list_audio(const fs::path& project, bool music) {
    std::vector<std::string> names;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(audio_folder(project, music), error))
        if (entry.is_regular_file() && valid_audio_file(entry.path().filename().string(), music))
            names.push_back(entry.path().filename().string());
    std::sort(names.begin(), names.end());
    return names;
}

std::string import_audio(const fs::path& project, const fs::path& source, bool music) {
    auto extension = source.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension != ".ogg" && (music || extension != ".wav"))
        throw std::runtime_error(music ? "Music is an Ogg Vorbis (.ogg) file"
                                       : "Sounds are WAV or Ogg Vorbis files");
    const auto bytes = read_text(source, 16 * 1024 * 1024);
    if (music)
        MusicStream check(bytes, false); // Refuses anything that will not decode, before copying it.
    else
        decode_sound(bytes);
    std::string base;
    for (const char c : source.stem().string())
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')
            base += c;
        else if (c == ' ' || c == '.')
            base += '_';
    if (base.empty()) base = music ? "music" : "sound";
    if (base.size() > 60) base.resize(60);
    const auto names = list_audio(project, music);
    const auto taken = [&](const std::string& stem) {
        return std::any_of(names.begin(), names.end(),
                           [&](const std::string& n) { return fs::path(n).stem() == stem; });
    };
    auto stem = base;
    for (int n = 2; taken(stem); ++n)
        stem = base + "_" + std::to_string(n);
    fs::create_directories(audio_folder(project, music));
    fs::copy_file(source, audio_folder(project, music) / (stem + extension));
    return stem + extension;
}
} // namespace seed::editor
