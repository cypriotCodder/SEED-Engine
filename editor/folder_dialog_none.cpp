#include "folder_dialog.hpp"

namespace seed::editor {
// Windows and Linux dialogs arrive with those platforms; until then the editor hides its Browse
// buttons and accepts typed paths.
bool folder_dialog_available() {
    return false;
}

std::optional<std::filesystem::path> choose_folder(const char*, const std::filesystem::path&) {
    return std::nullopt;
}
std::optional<std::filesystem::path> choose_image(const char*) {
    return std::nullopt;
}
std::optional<std::filesystem::path> choose_audio(const char*, bool) {
    return std::nullopt;
}
} // namespace seed::editor
