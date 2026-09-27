#pragma once
#include <filesystem>
#include <optional>

namespace seed::editor {
// Whether this platform has a native folder picker. Where it does not, the editor offers only a
// typed path.
bool folder_dialog_available();
// Shows the native folder picker and blocks until the user chooses a folder or cancels.
std::optional<std::filesystem::path> choose_folder(const char* title, const std::filesystem::path& start);
} // namespace seed::editor
