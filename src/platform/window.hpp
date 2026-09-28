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
    // A window of `width` x `height` logical pixels, or covering the screen when `fullscreen`.
    explicit Window(bool vsync = true, int width = 1280, int height = 720, bool fullscreen = false);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    void poll(Input& input);
    void clear(float red, float green, float blue);
    void present();
    void title(std::string_view value);
    void drawable_size(int& width, int& height) const;
    void logical_size(int& width, int& height) const;
    // Every polled event is also passed to `observer`, before Input sees it. One observer at a time.
    void observe(void* context, void (*observer)(void*, const SDL_Event&)) {
        observer_context_ = context;
        observer_ = observer;
    }
    SDL_Window* handle() const { return window_; }
    SDL_GLContext context() const { return context_; }

private:
    void release() noexcept;
    SDL_Window* window_{};
    SDL_GLContext context_{};
    void* observer_context_{};
    void (*observer_)(void*, const SDL_Event&){};
    using ViewportFn = void(APIENTRY*)(int, int, int, int);
    using ClearColorFn = void(APIENTRY*)(float, float, float, float);
    using ClearFn = void(APIENTRY*)(unsigned int);
    ViewportFn viewport_{};
    ClearColorFn clear_color_{};
    ClearFn clear_{};
};

} // namespace seed
