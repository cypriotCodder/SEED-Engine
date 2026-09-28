#include "platform/window.hpp"
#include <SDL_opengl.h>

#include <stdexcept>
#include <string>

namespace seed {
namespace {
[[noreturn]] void fail(const char* operation) {
    throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
}
void attribute(SDL_GLattr key, int value) {
    if (SDL_GL_SetAttribute(key, value) != 0) fail("Set OpenGL attribute");
}
template<class T>
T load(const char* name) {
    auto result = reinterpret_cast<T>(SDL_GL_GetProcAddress(name));
    if (!result) throw std::runtime_error(std::string("Missing OpenGL function: ") + name);
    return result;
}
} // namespace

Window::Window(bool vsync, int width, int height, bool fullscreen) {
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) fail("Initialize SDL");
    try {
        attribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
        attribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
        attribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        attribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
        attribute(SDL_GL_DOUBLEBUFFER, 1);
        if (width < 160 || height < 120 || width > 16384 || height > 16384)
            throw std::invalid_argument("Window size out of range");
        window_ =
            SDL_CreateWindow("Seed Engine", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height,
                             SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                                 (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
        if (!window_) fail("Create window");
        context_ = SDL_GL_CreateContext(window_);
        if (!context_) fail("Create OpenGL 4.1 context");
        int major{}, minor{};
        if (SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, &major) != 0 ||
            SDL_GL_GetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, &minor) != 0)
            fail("Read OpenGL version");
        if (major < 4 || (major == 4 && minor < 1))
            throw std::runtime_error("OpenGL 4.1 or later is required");
        viewport_ = load<ViewportFn>("glViewport");
        clear_color_ = load<ClearColorFn>("glClearColor");
        clear_ = load<ClearFn>("glClear");
#ifdef __APPLE__
        // SDL2's Cocoa display-link wait can stop progressing while the display sleeps.
        // The client loop paces frames explicitly on this development platform.
        vsync = false;
#endif
        if (SDL_GL_SetSwapInterval(vsync ? 1 : 0) != 0) SDL_Log("VSync unavailable: %s", SDL_GetError());
        SDL_Log("OpenGL context %d.%d", major, minor);
    } catch (...) {
        release();
        throw;
    }
}
Window::~Window() {
    release();
}
void Window::release() noexcept {
    if (context_) SDL_GL_DeleteContext(context_);
    if (window_) SDL_DestroyWindow(window_);
    context_ = nullptr;
    window_ = nullptr;
    SDL_Quit();
}
void Window::poll(Input& input) {
    input.pressed.fill(false);
    input.released.fill(false);
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (observer_) observer_(observer_context_, event);
        if (event.type == SDL_QUIT) input.quit = true;
        if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
            const auto key = static_cast<std::size_t>(event.key.keysym.scancode);
            if (key >= input.held.size()) continue;
            const bool down = event.type == SDL_KEYDOWN;
            if (down && !input.held[key]) input.pressed[key] = true;
            if (!down && input.held[key]) input.released[key] = true;
            input.held[key] = down;
        }
        if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            for (std::size_t i = 0; i < input.held.size(); ++i) {
                input.released[i] = input.released[i] || input.held[i];
                input.held[i] = false;
            }
        }
    }
    input.mouse_buttons = SDL_GetMouseState(&input.mouse_x, &input.mouse_y);
}
void Window::clear(float red, float green, float blue) {
    int width{}, height{};
    SDL_GL_GetDrawableSize(window_, &width, &height);
    viewport_(0, 0, width, height);
    clear_color_(red, green, blue, 1.0F);
    clear_(GL_COLOR_BUFFER_BIT);
}
void Window::present() {
    SDL_GL_SwapWindow(window_);
}
void Window::drawable_size(int& width, int& height) const {
    SDL_GL_GetDrawableSize(window_, &width, &height);
}
void Window::logical_size(int& width, int& height) const {
    SDL_GetWindowSize(window_, &width, &height);
}
void Window::title(std::string_view value) {
    const std::string terminated(value);
    SDL_SetWindowTitle(window_, terminated.c_str());
}
} // namespace seed
