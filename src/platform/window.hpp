#pragma once

#include <SDL.h>
#include <SDL_opengl.h>
#include <array>
#include <string_view>

namespace seed {

struct Input {
    std::array<bool, SDL_NUM_SCANCODES> held{};
    std::array<bool, SDL_NUM_SCANCODES> pressed{};
    std::array<bool, SDL_NUM_SCANCODES> released{};
    int mouse_x{};
    int mouse_y{};
    Uint32 mouse_buttons{};
    bool quit{};
};

// Owns SDL video and a single GL context. All methods run on the main thread.
class Window final {
public:
    explicit Window(bool vsync = true);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    void poll(Input& input);
    void clear(float red, float green, float blue);
    void present();
    void title(std::string_view value);
    void drawable_size(int& width, int& height) const;
    void logical_size(int& width, int& height) const;

private:
    void release() noexcept;
    SDL_Window* window_{};
    SDL_GLContext context_{};
    using ViewportFn = void(APIENTRY*)(int, int, int, int);
    using ClearColorFn = void(APIENTRY*)(float, float, float, float);
    using ClearFn = void(APIENTRY*)(unsigned int);
    ViewportFn viewport_{};
    ClearColorFn clear_color_{};
    ClearFn clear_{};
};

} // namespace seed
