#pragma once
#include "assets/pack.hpp"
#include "render/gl.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace seed {
struct Sprite {
    float x{}, y{}, width{1}, height{1};
    float u0{}, v0{}, u1{}, v1{};
    float red{1}, green{1}, blue{1}, alpha{1};
    float angle{};
};
enum class Material : unsigned { water, sand, grass, stone, wood, leaves, player, ember, count };

class Renderer final {
public:
    static constexpr std::size_t capacity = 32768;
    explicit Renderer(const Pack& pack);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    void begin(int width, int height, float camera_x, float camera_y, float pixels_per_unit);
    void sprite(Material material, float x, float y, float width = 1, float height = 1, float angle = 0,
                float shade = 1);
    void flush();
    void finish();
    void light(float x, float y, float radius, float red, float green, float blue, float intensity = 2,
               float height = 2);
    void screenshot(const char* path, int width, int height);
    [[nodiscard]] unsigned draw_calls() const { return calls_; }

private:
    void release() noexcept;
    GLuint shader(GLenum type, const char* source);
    GLuint link_program(const char* vertex_source, const char* fragment_source);
    void resize_targets(int width, int height);
    Gl gl_;
    GLuint vao_{}, buffer_{}, program_{}, atlas_{}, detail_{};
    bool using_detail_{};
    struct Light {
        float x{}, y{}, radius{}, red{}, green{}, blue{}, intensity{}, height{};
    };
    std::array<Light, 32> lights_{};
    std::size_t light_count_{};
    GLuint normals_{}, geometry_fbo_{}, light_fbo_{}, albedo_target_{}, normal_target_{}, light_target_{};
    GLuint light_program_{}, composite_program_{};
    GLint normal_enabled_{}, light_rect_{}, light_position_{}, light_view_{}, light_color_{};
    int width_{}, height_{};
    float view_width_{}, view_height_{};
    GLint camera_uniform_{}, scale_uniform_{};
    std::unique_ptr<Sprite[]> sprites_;
    std::size_t size_{};
    unsigned calls_{};
};
} // namespace seed
