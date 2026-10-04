#include "splash.hpp"
#include "textures.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <imgui.h>

namespace seed::editor {
extern const unsigned char splash_jpg[], icon_jpg[];
extern const std::size_t splash_jpg_size, icon_jpg_size;

namespace {
constexpr double shown_seconds = 1.6;
constexpr ImU32 ink = IM_COL32(46, 91, 208, 255); // The logo's blue.
} // namespace

void set_window_icon(SDL_Window* window) {
    int width{}, height{};
    auto pixels = decode_image(icon_jpg, icon_jpg_size, width, height);
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(pixels.data(), width, height, 32, width * 4,
                                                              SDL_PIXELFORMAT_RGBA32);
    if (!surface) return; // The window keeps the system's default icon.
    SDL_SetWindowIcon(window, surface);
    SDL_FreeSurface(surface);
}

Splash::Splash(SDL_Window* window) : window_(window) {
    // The window starts as a small borderless card in the middle of the screen, and grows into
    // the editor when the screen is done.
    SDL_GetWindowSize(window_, &restore_width_, &restore_height_);
    SDL_SetWindowBordered(window_, SDL_FALSE);
    SDL_SetWindowResizable(window_, SDL_FALSE);
    SDL_SetWindowSize(window_, splash_width, splash_height);
    SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    auto pixels = decode_image(splash_jpg, splash_jpg_size, width_, height_);
    // The screen around the artwork takes the average colour of its border, and the artwork's
    // outer margin fades into that colour, so no edge shows however its paper is textured.
    const auto at = [&](int x, int y) {
        return pixels.data() + (std::size_t(y) * std::size_t(width_) + std::size_t(x)) * 4;
    };
    double sum[3]{};
    int count = 0;
    const auto add = [&](int x, int y) {
        for (int c = 0; c < 3; ++c)
            sum[c] += at(x, y)[c];
        ++count;
    };
    for (int x = 0; x < width_; ++x)
        add(x, 0), add(x, height_ - 1);
    for (int y = 0; y < height_; ++y)
        add(0, y), add(width_ - 1, y);
    for (int c = 0; c < 3; ++c)
        paper_[c] = static_cast<float>(sum[c] / count / 255);
    const float margin = static_cast<float>(std::min(width_, height_)) * 0.18F;
    for (int y = 0; y < height_; ++y)
        for (int x = 0; x < width_; ++x) {
            const float edge = static_cast<float>(std::min({x, y, width_ - 1 - x, height_ - 1 - y}));
            if (edge >= margin) continue;
            float t = edge / margin;
            t = t * t * (3 - 2 * t); // 0 at the edge, 1 inside the margin.
            auto* p = at(x, y);
            for (int c = 0; c < 3; ++c)
                p[c] = static_cast<std::uint8_t>(std::lround(paper_[c] * 255 * (1 - t) + p[c] * t));
        }
    gl_.GenTextures(1, &texture_);
    gl_.BindTexture(GL_TEXTURE_2D, texture_);
    gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl_.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width_, height_, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    gl_.BindTexture(GL_TEXTURE_2D, 0);
}

Splash::~Splash() {
    if (texture_) gl_.DeleteTextures(1, &texture_);
}

void Splash::draw(const std::string& status) {
    if (!active_) return;
    const double now = ImGui::GetTime();
    if (start_ < 0) start_ = now;
    const bool skip = ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                      ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_Escape) ||
                      ImGui::IsKeyPressed(ImGuiKey_Space) || ImGui::IsKeyPressed(ImGuiKey_Enter);
    if (skip || now - start_ >= shown_seconds) return finish();
    // The launch screen fills its small window, and takes the input while it shows.
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::SetNextWindowFocus();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::SetNextWindowBgAlpha(0);
    ImGui::Begin("##splash", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar(2);
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 lo = viewport->Pos, size = viewport->Size, hi{lo.x + size.x, lo.y + size.y};
    draw->AddRectFilled(lo, hi, ImGui::ColorConvertFloat4ToU32({paper_[0], paper_[1], paper_[2], 1}));
    // The artwork across the top 85%, cropped (it is mostly paper) to that shape rather than
    // squeezed; its lower part holds the text below.
    const float art_h = size.y * 0.85F;
    const float window_aspect = size.x / art_h,
                art_aspect = static_cast<float>(width_) / static_cast<float>(height_);
    ImVec2 uv0{0, 0}, uv1{1, 1};
    if (art_aspect > window_aspect) {
        const float keep = window_aspect / art_aspect; // Share of the width shown.
        uv0.x = (1 - keep) / 2, uv1.x = 1 - uv0.x;
    } else {
        const float keep = art_aspect / window_aspect;
        uv0.y = (1 - keep) / 2, uv1.y = 1 - uv0.y;
    }
    draw->AddImage(static_cast<ImTextureID>(texture_), lo, {hi.x, lo.y + art_h}, uv0, uv1);
    // The version, what is happening, and a thin bar that fills while the screen shows.
    const float centre = lo.x + size.x / 2;
    const std::string version = std::string("Version ") + editor_version;
    const auto vs = ImGui::CalcTextSize(version.c_str()), ss = ImGui::CalcTextSize(status.c_str());
    const float bar_y = hi.y - 22, status_y = bar_y - 10 - ss.y, version_y = status_y - 4 - vs.y;
    draw->AddText({centre - vs.x / 2, version_y}, (ink & 0x00FFFFFF) | 0xE6000000, version.c_str());
    draw->AddText({centre - ss.x / 2, status_y}, (ink & 0x00FFFFFF) | 0x8C000000, status.c_str());
    const float progress = static_cast<float>(std::min(1.0, (now - start_) / shown_seconds));
    const float bar = 200;
    draw->AddRectFilled({centre - bar / 2, bar_y}, {centre + bar / 2, bar_y + 3},
                        (ink & 0x00FFFFFF) | 0x1F000000, 2);
    draw->AddRectFilled({centre - bar / 2, bar_y}, {centre - bar / 2 + bar * progress, bar_y + 3}, ink, 2);
    ImGui::End();
}

void Splash::finish() {
    active_ = false;
    gl_.DeleteTextures(1, &texture_);
    texture_ = 0;
    // Back to the editor's own window.
    SDL_SetWindowBordered(window_, SDL_TRUE);
    SDL_SetWindowResizable(window_, SDL_TRUE);
    SDL_SetWindowSize(window_, restore_width_, restore_height_);
    SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}
} // namespace seed::editor
