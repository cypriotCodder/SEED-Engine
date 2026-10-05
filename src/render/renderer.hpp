#pragma once
#include "assets/pack.hpp"
#include "core/material.hpp"
#include "render/gl.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace seed {
struct Sprite {
    float x{}, y{}, width{1}, height{1};
    float u0{}, v0{}, u1{}, v1{};
    float red{1}, green{1}, blue{1}, alpha{1};
    float angle{};
};

struct Color {
    float red{1}, green{1}, blue{1}, alpha{1};
};

// Scene-wide lighting. Games may change it at any time; it applies from the next frame.
struct Lighting {
    std::array<float, 3> clear{0.035F, 0.065F, 0.085F}; // Background where nothing is drawn.
    std::array<float, 3> ambient{0.34F, 0.43F, 0.56F};  // Light present everywhere.
    std::array<float, 3> haze{0.085F, 0.13F, 0.17F};    // Colour blended in towards the edges.
    float haze_amount{0.2F};                            // 0 disables the edge haze.
};

class Renderer final {
public:
    static constexpr std::size_t capacity = 32768;
    // Builds the atlas from every registered material; materials with a texture load it from `pack`.
    Renderer(const Pack& pack, const Materials& materials);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    void begin(int width, int height, float camera_x, float camera_y, float pixels_per_unit);
    // For animated materials (see MaterialDesc::frames), `frame` picks the picture; `automatic`
    // follows the clock set with set_time.
    static constexpr unsigned automatic = ~0U;
    void sprite(MaterialId material, float x, float y, float width = 1, float height = 1, float angle = 0,
                float shade = 1, unsigned frame = automatic);
    // Seconds of game time that animated materials play by; set once a frame.
    void set_time(double seconds) { time_ = seconds; }
    // A ground tile whose lower-left corner is at global tile coordinates (gx, gy), drawn at
    // (x, y) like sprite(). A textured material shows the part of its texture that falls on this
    // tile, so the texture continues across neighbouring tiles; other materials draw as sprite().
    void ground(MaterialId material, float x, float y, double gx, double gy);
    void flush();
    void finish();
    // Where finish() places the frame in the window's framebuffer, in pixels from its bottom-left
    // corner. Games leave it at the origin; the editor draws its scene view inside a panel.
    void set_output(int x, int y) { output_x_ = x, output_y_ = y; }
    void light(float x, float y, float radius, float red, float green, float blue, float intensity = 2,
               float height = 2);
    void screenshot(const char* path, int width, int height);
    [[nodiscard]] unsigned draw_calls() const { return calls_; }

    // Screen-space UI, drawn unlit on top of the lit scene in the order submitted. Coordinates are
    // logical pixels from the top-left corner (see set_ui_scale).
    static constexpr std::size_t ui_capacity = 8192;
    void ui_rect(float x, float y, float width, float height, Color color);
    // Draws ASCII text with the built-in 5x7 font, 6x8 logical pixels per character at scale 1;
    // '\n' starts a new line. Returns the width of the widest line. Non-ASCII bytes draw as '?'.
    float text(float x, float y, std::string_view value, float scale = 2, Color color = {});
    static float text_width(std::string_view value, float scale = 2);
    // Drawable pixels per logical pixel; the engine sets it each frame for high-DPI displays.
    void set_ui_scale(float scale) { ui_scale_ = scale; }

    Lighting lighting;

private:
    void release() noexcept;
    GLuint shader(GLenum type, const char* source);
    GLuint link_program(const char* vertex_source, const char* fragment_source);
    void resize_targets(int width, int height);
    Gl gl_;
    GLuint vao_{}, buffer_{}, program_{}, atlas_{};
    std::array<GLuint, Materials::capacity> textures_{};      // Per material; 0 draws from the atlas.
    std::array<float, Materials::capacity> texture_scales_{}; // Tiles per texture copy.
    std::array<std::uint8_t, Materials::capacity> frames_{};  // Animation frames; 0 or 1 for none.
    std::array<float, Materials::capacity> fps_{}, frame_inset_{};
    double time_{};
    unsigned frame_of(MaterialId material, unsigned frame) const;
    GLuint bound_{}; // Texture currently bound for sprites.
    std::size_t material_count_{};
    float atlas_width_{};
    struct Light {
        float x{}, y{}, radius{}, red{}, green{}, blue{}, intensity{}, height{};
    };
    std::array<Light, 32> lights_{};
    std::size_t light_count_{};
    GLuint normals_{}, geometry_fbo_{}, light_fbo_{}, albedo_target_{}, normal_target_{}, light_target_{};
    GLuint light_program_{}, composite_program_{}, ui_program_{}, font_{};
    GLint haze_uniform_{}, ui_screen_uniform_{};
    std::unique_ptr<Sprite[]> ui_sprites_;
    std::size_t ui_size_{};
    float ui_scale_{1};
    void ui_quad(float x, float y, float width, float height, float u0, float v0, float u1, float v1,
                 Color color);
    GLint normal_enabled_{}, light_rect_{}, light_position_{}, light_view_{}, light_color_{};
    int width_{}, height_{}, output_x_{}, output_y_{};
    float view_width_{}, view_height_{};
    GLint camera_uniform_{}, scale_uniform_{};
    std::unique_ptr<Sprite[]> sprites_;
    std::size_t size_{};
    unsigned calls_{};
};
} // namespace seed
