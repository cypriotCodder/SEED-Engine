#include "render/renderer.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <vector>

namespace seed {
namespace {
constexpr int tile = 32, pitch = tile + 2;
constexpr const char* vertex = R"GLSL(#version 410 core
layout(location=0) in vec4 rectangle;
layout(location=1) in vec4 region;
layout(location=2) in vec4 tint;
layout(location=3) in float angle;
uniform vec2 camera;
uniform vec2 scale;
out vec2 uv;
out vec4 color;
out vec2 rotation;
void main() {
    const vec2 corner[6] = vec2[6](vec2(0,0),vec2(1,0),vec2(1,1),vec2(0,0),vec2(1,1),vec2(0,1));
    vec2 q = corner[gl_VertexID];
    vec2 p = (q - 0.5) * rectangle.zw;
    float c = cos(angle), s = sin(angle);
    p = mat2(c,s,-s,c) * p + rectangle.xy;
    gl_Position = vec4((p-camera)*scale,0,1);
    uv = mix(region.xy,region.zw,q);
    color = tint;
    rotation = vec2(c,s);
})GLSL";
constexpr const char* fragment = R"GLSL(#version 410 core
in vec2 uv;
in vec4 color;
in vec2 rotation;
uniform sampler2D atlas;
uniform sampler2D normalAtlas;
uniform int normalEnabled;
layout(location=0) out vec4 pixel;
layout(location=1) out vec4 surface;
void main() {
    pixel = texture(atlas,uv) * color;
    if (pixel.a < 0.01) discard;
    vec3 n = normalEnabled != 0 ? texture(normalAtlas,uv).xyz*2.0-1.0 : vec3(0,0,1);
    n.xy = mat2(rotation.x,rotation.y,-rotation.y,rotation.x)*n.xy;
    surface = vec4(normalize(n)*0.5+0.5,pixel.a);
})GLSL";
constexpr const char* screen_vertex = R"GLSL(#version 410 core
uniform vec4 rectangle;
out vec2 uv;
void main() {
    const vec2 corner[6]=vec2[6](vec2(0,0),vec2(1,0),vec2(1,1),vec2(0,0),vec2(1,1),vec2(0,1));
    uv=mix(rectangle.xy,rectangle.zw,corner[gl_VertexID]);
    gl_Position=vec4(uv*2.0-1.0,0,1);
})GLSL";
constexpr const char* light_fragment = R"GLSL(#version 410 core
in vec2 uv;
uniform sampler2D normals;
uniform vec4 lightPosition;
uniform vec4 lightColor;
uniform vec2 viewSize;
out vec4 pixel;
void main() {
    vec2 delta=(lightPosition.xy-uv)*viewSize;
    float radial=length(delta)/lightPosition.z;
    if (radial>=1.0) discard;
    vec3 n=normalize(texture(normals,uv).xyz*2.0-1.0);
    vec3 direction=normalize(vec3(delta,lightPosition.w));
    float diffuse=max(0.0,dot(n,direction));
    float attenuation=(1.0-radial)*(1.0-radial);
    pixel=vec4(lightColor.rgb*lightColor.a*attenuation*diffuse,0);
})GLSL";
constexpr const char* composite_fragment = R"GLSL(#version 410 core
in vec2 uv;
uniform sampler2D albedo;
uniform sampler2D illumination;
out vec4 pixel;
void main() {
    vec3 base=pow(texture(albedo,uv).rgb,vec3(2.2));
    vec3 lit=base*texture(illumination,uv).rgb;
    lit=1.0-exp(-lit);
    float fog=smoothstep(0.2,0.8,length(uv-0.5))*0.2;
    pixel=vec4(mix(pow(lit,vec3(1.0/2.2)),vec3(0.085,0.13,0.17),fog),1);
})GLSL";
std::uint32_t hash(std::uint32_t n) {
    n ^= n >> 16;
    n *= 0x7feb352dU;
    n ^= n >> 15;
    n *= 0x846ca68bU;
    return n ^ (n >> 16);
}
} // namespace
GLuint Renderer::shader(GLenum type, const char* source) {
    const auto result = gl_.CreateShader(type);
    gl_.ShaderSource(result, 1, &source, nullptr);
    gl_.CompileShader(result);
    GLint ok{};
    gl_.GetShaderiv(result, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        std::array<char, 4096> message{};
        gl_.GetShaderInfoLog(result, static_cast<GLsizei>(message.size()), nullptr, message.data());
        gl_.DeleteShader(result);
        throw std::runtime_error(std::string("Shader compilation: ") + message.data());
    }
    return result;
}
GLuint Renderer::link_program(const char* vertex_source, const char* fragment_source) {
    GLuint vs = 0, fs = 0, result = 0;
    try {
        vs = shader(GL_VERTEX_SHADER, vertex_source);
        fs = shader(GL_FRAGMENT_SHADER, fragment_source);
        result = gl_.CreateProgram();
        gl_.AttachShader(result, vs);
        gl_.AttachShader(result, fs);
        gl_.LinkProgram(result);
        GLint ok{};
        gl_.GetProgramiv(result, GL_LINK_STATUS, &ok);
        if (!ok) {
            std::array<char, 4096> log{};
            gl_.GetProgramInfoLog(result, static_cast<GLsizei>(log.size()), nullptr, log.data());
            throw std::runtime_error(std::string("Lighting shader link: ") + log.data());
        }
        gl_.DeleteShader(vs);
        gl_.DeleteShader(fs);
        return result;
    } catch (...) {
        if (vs) gl_.DeleteShader(vs);
        if (fs) gl_.DeleteShader(fs);
        if (result) gl_.DeleteProgram(result);
        throw;
    }
}
Renderer::Renderer(const Pack& pack, const Materials& registry)
    : material_count_(registry.size()), sprites_(std::make_unique<Sprite[]>(capacity)) {
    if (!material_count_) throw std::invalid_argument("Register at least one material before rendering");
    const int materials = static_cast<int>(material_count_);
    const int atlas_width = pitch * materials;
    atlas_width_ = static_cast<float>(atlas_width);
    GLuint vs{}, fs{};
    try {
        vs = shader(GL_VERTEX_SHADER, vertex);
        fs = shader(GL_FRAGMENT_SHADER, fragment);
        program_ = gl_.CreateProgram();
        gl_.AttachShader(program_, vs);
        gl_.AttachShader(program_, fs);
        gl_.LinkProgram(program_);
        gl_.DeleteShader(vs);
        vs = 0;
        gl_.DeleteShader(fs);
        fs = 0;
        GLint ok{};
        gl_.GetProgramiv(program_, GL_LINK_STATUS, &ok);
        if (!ok) {
            std::array<char, 4096> message{};
            gl_.GetProgramInfoLog(program_, static_cast<GLsizei>(message.size()), nullptr, message.data());
            throw std::runtime_error(std::string("Shader link: ") + message.data());
        }
        camera_uniform_ = gl_.GetUniformLocation(program_, "camera");
        scale_uniform_ = gl_.GetUniformLocation(program_, "scale");
        gl_.GenVertexArrays(1, &vao_);
        gl_.BindVertexArray(vao_);
        gl_.GenBuffers(1, &buffer_);
        gl_.BindBuffer(GL_ARRAY_BUFFER, buffer_);
        gl_.BufferData(GL_ARRAY_BUFFER, capacity * sizeof(Sprite), nullptr, GL_STREAM_DRAW);
        constexpr std::array<std::size_t, 4> offsets{offsetof(Sprite, x), offsetof(Sprite, u0),
                                                     offsetof(Sprite, red), offsetof(Sprite, angle)};
        for (GLuint i = 0; i < offsets.size(); ++i) {
            gl_.EnableVertexAttribArray(i);
            gl_.VertexAttribPointer(i, i == 3 ? 1 : 4, GL_FLOAT, GL_FALSE, sizeof(Sprite),
                                    reinterpret_cast<const void*>(offsets[i]));
            gl_.VertexAttribDivisor(i, 1);
        }
        std::vector<std::uint8_t> pixels(atlas_width * pitch * 4);
        for (int m = 0; m < materials; ++m) {
            for (int y = 0; y < pitch; ++y)
                for (int x = 0; x < pitch; ++x) {
                    const int px = std::clamp(x - 1, 0, tile - 1), py = std::clamp(y - 1, 0, tile - 1);
                    const auto& recipe = registry[static_cast<MaterialId>(m)];
                    const auto noise = hash(static_cast<std::uint32_t>(m * 991 + py * tile + px));
                    int variation = static_cast<int>(noise % static_cast<std::uint32_t>(recipe.variation)) -
                                    recipe.variation / 2;
                    if (recipe.pattern == Pattern::planks)
                        variation += (py % 8 == 0 ? -32 : 0) + (px % 11 == 0 ? -10 : 0);
                    if (recipe.pattern == Pattern::water) variation = (py % 9 == 0 ? 13 : variation / 3);
                    const auto index = static_cast<std::size_t>((y * atlas_width + m * pitch + x) * 4);
                    for (int c = 0; c < 3; ++c)
                        pixels[index + c] =
                            static_cast<std::uint8_t>(std::clamp(recipe.color[c] + variation, 0, 255));
                    const float dx = (static_cast<float>(px) - 15.5F) / 16.0F;
                    const float dy = (static_cast<float>(py) - 15.5F) / 16.0F;
                    pixels[index + 3] =
                        (recipe.pattern == Pattern::round && dx * dx + dy * dy > 0.9F) ? 0 : 255;
                }
        }
        gl_.GenTextures(1, &atlas_);
        gl_.BindTexture(GL_TEXTURE_2D, atlas_);
        gl_.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, atlas_width, pitch, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                       pixels.data());
        gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        for (std::size_t m = 0; m < material_count_; ++m) {
            const auto* name = registry[static_cast<MaterialId>(m)].texture;
            if (!name) continue;
            const auto& image = pack.texture(name);
            gl_.GenTextures(1, &textures_[m]);
            gl_.BindTexture(GL_TEXTURE_2D, textures_[m]);
            gl_.CompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT,
                                     static_cast<GLsizei>(image.width), static_cast<GLsizei>(image.height), 0,
                                     static_cast<GLsizei>(image.blocks.size()), image.blocks.data());
            gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        std::vector<std::uint8_t> normal_pixels(pixels.size());
        for (int m = 0; m < materials; ++m)
            for (int y = 0; y < pitch; ++y)
                for (int x = 0; x < pitch; ++x) {
                    const float dx = (static_cast<float>(std::clamp(x - 1, 0, tile - 1)) - 15.5F) / 16;
                    const float dy = (static_cast<float>(std::clamp(y - 1, 0, tile - 1)) - 15.5F) / 16;
                    float nx = 0, ny = 0, nz = 1;
                    const auto pattern = registry[static_cast<MaterialId>(m)].pattern;
                    if (pattern == Pattern::round) {
                        nx = dx * 0.7F;
                        ny = dy * 0.7F;
                        nz = std::sqrt(std::max(0.05F, 1 - nx * nx - ny * ny));
                    } else if (pattern == Pattern::planks) {
                        ny = 0.3F * std::sin(static_cast<float>(y) * 0.8F);
                    } else {
                        nx = 0.05F * std::sin(static_cast<float>(x) * 2);
                        ny = 0.05F * std::sin(static_cast<float>(y) * 3);
                    }
                    const float inverse = 1 / std::sqrt(nx * nx + ny * ny + nz * nz);
                    const auto i = static_cast<std::size_t>((y * atlas_width + m * pitch + x) * 4);
                    normal_pixels[i] = static_cast<std::uint8_t>((nx * inverse * 0.5F + 0.5F) * 255);
                    normal_pixels[i + 1] = static_cast<std::uint8_t>((ny * inverse * 0.5F + 0.5F) * 255);
                    normal_pixels[i + 2] = static_cast<std::uint8_t>((nz * inverse * 0.5F + 0.5F) * 255);
                    normal_pixels[i + 3] = 255;
                }
        gl_.GenTextures(1, &normals_);
        gl_.BindTexture(GL_TEXTURE_2D, normals_);
        gl_.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, atlas_width, pitch, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                       normal_pixels.data());
        gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl_.UseProgram(program_);
        gl_.Uniform1i(gl_.GetUniformLocation(program_, "normalAtlas"), 1);
        normal_enabled_ = gl_.GetUniformLocation(program_, "normalEnabled");
        light_program_ = link_program(screen_vertex, light_fragment);
        light_rect_ = gl_.GetUniformLocation(light_program_, "rectangle");
        light_position_ = gl_.GetUniformLocation(light_program_, "lightPosition");
        light_view_ = gl_.GetUniformLocation(light_program_, "viewSize");
        light_color_ = gl_.GetUniformLocation(light_program_, "lightColor");
        composite_program_ = link_program(screen_vertex, composite_fragment);
        gl_.UseProgram(composite_program_);
        gl_.Uniform1i(gl_.GetUniformLocation(composite_program_, "albedo"), 0);
        gl_.Uniform1i(gl_.GetUniformLocation(composite_program_, "illumination"), 1);
        gl_.Uniform4f(gl_.GetUniformLocation(composite_program_, "rectangle"), 0, 0, 1, 1);
        gl_.GenFramebuffers(1, &geometry_fbo_);
        gl_.GenFramebuffers(1, &light_fbo_);
        gl_.GenTextures(1, &albedo_target_);
        gl_.GenTextures(1, &normal_target_);
        gl_.GenTextures(1, &light_target_);
        gl_.check();
    } catch (...) {
        if (vs) gl_.DeleteShader(vs);
        if (fs) gl_.DeleteShader(fs);
        release();
        throw;
    }
}
Renderer::~Renderer() {
    release();
}
void Renderer::release() noexcept {
    gl_.DeleteTextures(1, &atlas_);
    gl_.DeleteBuffers(1, &buffer_);
    for (auto& texture : textures_)
        if (texture) gl_.DeleteTextures(1, &texture);
    gl_.DeleteTextures(1, &normals_);
    gl_.DeleteTextures(1, &albedo_target_);
    gl_.DeleteTextures(1, &normal_target_);
    gl_.DeleteTextures(1, &light_target_);
    gl_.DeleteFramebuffers(1, &geometry_fbo_);
    gl_.DeleteFramebuffers(1, &light_fbo_);
    if (light_program_) gl_.DeleteProgram(light_program_);
    if (composite_program_) gl_.DeleteProgram(composite_program_);
    gl_.DeleteVertexArrays(1, &vao_);
    if (program_) gl_.DeleteProgram(program_);
}
void Renderer::resize_targets(int width, int height) {
    if (width_ == width && height_ == height) return;
    auto texture = [&](GLuint id, int w, int h, GLint format) {
        gl_.BindTexture(GL_TEXTURE_2D, id);
        gl_.TexImage2D(GL_TEXTURE_2D, 0, format, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
        gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    texture(albedo_target_, width, height, GL_RGBA8);
    texture(normal_target_, width, height, GL_RGBA8);
    texture(light_target_, std::max(1, width / 2), std::max(1, height / 2), GL_RGBA16F);
    gl_.BindFramebuffer(GL_FRAMEBUFFER, geometry_fbo_);
    gl_.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, albedo_target_, 0);
    gl_.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, normal_target_, 0);
    const std::array<GLenum, 2> outputs{GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    gl_.DrawBuffers(2, outputs.data());
    if (gl_.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        throw std::runtime_error("Incomplete geometry framebuffer");
    gl_.BindFramebuffer(GL_FRAMEBUFFER, light_fbo_);
    gl_.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, light_target_, 0);
    gl_.DrawBuffers(1, outputs.data());
    if (gl_.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        throw std::runtime_error("Incomplete light framebuffer");
    width_ = width;
    height_ = height;
    gl_.check();
}
void Renderer::begin(int width, int height, float x, float y, float zoom) {
    if (width <= 0 || height <= 0 || zoom <= 0) throw std::invalid_argument("Invalid viewport");
    size_ = 0;
    calls_ = 0;
    bound_ = atlas_;
    light_count_ = 0;
    resize_targets(width, height);
    view_width_ = static_cast<float>(width) / zoom;
    view_height_ = static_cast<float>(height) / zoom;
    gl_.BindFramebuffer(GL_FRAMEBUFFER, geometry_fbo_);
    gl_.Viewport(0, 0, width, height);
    gl_.ClearColor(0.035F, 0.065F, 0.085F, 1);
    gl_.Clear(GL_COLOR_BUFFER_BIT);
    gl_.Enable(GL_BLEND);
    gl_.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl_.UseProgram(program_);
    gl_.BindVertexArray(vao_);
    gl_.Uniform1i(normal_enabled_, 1);
    gl_.ActiveTexture(GL_TEXTURE1);
    gl_.BindTexture(GL_TEXTURE_2D, normals_);
    gl_.ActiveTexture(GL_TEXTURE0);
    gl_.BindTexture(GL_TEXTURE_2D, atlas_);
    gl_.Uniform2f(camera_uniform_, x, y);
    gl_.Uniform2f(scale_uniform_, 2 * zoom / static_cast<float>(width),
                  2 * zoom / static_cast<float>(height));
}
void Renderer::sprite(MaterialId material, float x, float y, float width, float height, float angle,
                      float shade) {
    if (material >= material_count_) throw std::invalid_argument("Invalid material");
    // Textured materials draw their whole texture without normals; switching texture flushes.
    const bool detail = textures_[material] != 0;
    const GLuint texture = detail ? textures_[material] : atlas_;
    if (texture != bound_) {
        flush();
        bound_ = texture;
        gl_.ActiveTexture(GL_TEXTURE0);
        gl_.BindTexture(GL_TEXTURE_2D, texture);
        gl_.Uniform1i(normal_enabled_, detail ? 0 : 1);
    }
    if (size_ == capacity) flush();
    const float origin = static_cast<float>(static_cast<unsigned>(material) * pitch + 1);
    sprites_[size_++] = {x,
                         y,
                         width,
                         height,
                         origin / atlas_width_,
                         1.0F / pitch,
                         (origin + tile) / atlas_width_,
                         static_cast<float>(tile + 1) / pitch,
                         shade,
                         shade,
                         shade,
                         1,
                         angle};
    if (detail) {
        auto& sprite = sprites_[size_ - 1];
        sprite.u0 = sprite.v0 = 0;
        sprite.u1 = sprite.v1 = 1;
    }
}
void Renderer::flush() {
    if (!size_) return;
    gl_.BindBuffer(GL_ARRAY_BUFFER, buffer_);
    // Orphan the old store so the driver need not wait for the preceding frame.
    gl_.BufferData(GL_ARRAY_BUFFER, capacity * sizeof(Sprite), nullptr, GL_STREAM_DRAW);
    gl_.BufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(size_ * sizeof(Sprite)), sprites_.get());
    gl_.DrawArraysInstanced(GL_TRIANGLES, 0, 6, static_cast<GLsizei>(size_));
    size_ = 0;
    ++calls_;
}
void Renderer::light(float x, float y, float radius, float red, float green, float blue, float intensity,
                     float height) {
    if (light_count_ == lights_.size()) throw std::runtime_error("Visible light budget exhausted");
    if (radius <= 0 || height <= 0) throw std::invalid_argument("Light radius and height must be positive");
    lights_[light_count_++] = {x, y, radius, red, green, blue, intensity, height};
}
void Renderer::finish() {
    flush();
    gl_.BindFramebuffer(GL_FRAMEBUFFER, light_fbo_);
    gl_.Viewport(0, 0, std::max(1, width_ / 2), std::max(1, height_ / 2));
    gl_.ClearColor(0.34F, 0.43F, 0.56F, 1);
    gl_.Clear(GL_COLOR_BUFFER_BIT);
    gl_.UseProgram(light_program_);
    gl_.BlendFunc(GL_ONE, GL_ONE);
    gl_.ActiveTexture(GL_TEXTURE0);
    gl_.BindTexture(GL_TEXTURE_2D, normal_target_);
    gl_.Uniform2f(light_view_, view_width_, view_height_);
    for (std::size_t i = 0; i < light_count_; ++i) {
        const auto& l = lights_[i];
        const float x = 0.5F + l.x / view_width_, y = 0.5F + l.y / view_height_;
        gl_.Uniform4f(light_rect_, x - l.radius / view_width_, y - l.radius / view_height_,
                      x + l.radius / view_width_, y + l.radius / view_height_);
        gl_.Uniform4f(light_position_, x, y, l.radius, l.height);
        gl_.Uniform4f(light_color_, l.red, l.green, l.blue, l.intensity);
        gl_.DrawArraysInstanced(GL_TRIANGLES, 0, 6, 1);
        ++calls_;
    }
    gl_.BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl_.Viewport(0, 0, width_, height_);
    gl_.Disable(GL_BLEND);
    gl_.UseProgram(composite_program_);
    gl_.ActiveTexture(GL_TEXTURE0);
    gl_.BindTexture(GL_TEXTURE_2D, albedo_target_);
    gl_.ActiveTexture(GL_TEXTURE1);
    gl_.BindTexture(GL_TEXTURE_2D, light_target_);
    gl_.DrawArraysInstanced(GL_TRIANGLES, 0, 6, 1);
    ++calls_;
    gl_.check();
}
void Renderer::screenshot(const char* path, int width, int height) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
    gl_.ReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    gl_.check();
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << width << ' ' << height << "\n255\n";
    for (int y = height - 1; y >= 0; --y)
        for (int x = 0; x < width; ++x)
            out.write(
                reinterpret_cast<const char*>(pixels.data() + (static_cast<std::size_t>(y) * width + x) * 4),
                3);
    if (!out) throw std::runtime_error("Cannot write screenshot");
}
} // namespace seed
