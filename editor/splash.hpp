#pragma once
#include "render/gl.hpp"
#include <SDL.h>
#include <string>

namespace seed::editor {
// The editor's version, shown on the launch screen and in Help > About.
constexpr const char* editor_version = "0.01";

// Gives the window (and, on macOS, the Dock) the Seed icon.
void set_window_icon(SDL_Window* window);

// The launch screen: while the editor starts, its window is a borderless 600 x 375 card in the
// middle of the screen showing the Seed artwork, the version, what the editor is doing and a
// progress bar; then the window grows into the editor. Any click or key skips it. The artwork is
// embedded in the editor, so it needs no files beside it.
class Splash final {
public:
    static constexpr int splash_width = 600, splash_height = 375; // Logical pixels.
    explicit Splash(SDL_Window* window);
    ~Splash();
    Splash(const Splash&) = delete;
    Splash& operator=(const Splash&) = delete;
    bool active() const { return active_; }
    // Draws it over the frame; call last, between ImGui::NewFrame and ImGui::Render.
    void draw(const std::string& status);

private:
    void finish(); // Gives the window back its editor size and frame.
    SDL_Window* window_;
    int restore_width_{}, restore_height_{};
    Gl gl_;
    GLuint texture_{};
    int width_{}, height_{};
    float paper_[3]{1, 1, 1}; // The artwork's corner colour, so its edges disappear into the screen.
    double start_{-1};
    bool active_{true};
};
} // namespace seed::editor
