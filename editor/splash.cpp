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
constexpr double shown_seconds = 1.4, fade_seconds = 0.35;
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

Splash::Splash() {
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
    if (fade_from_ < 0 && (skip || now - start_ >= shown_seconds)) fade_from_ = now;
    const float alpha =
        fade_from_ < 0 ? 1.0F : 1.0F - static_cast<float>(std::min(1.0, (now - fade_from_) / fade_seconds));
    if (alpha <= 0) {
        active_ = false;
        gl_.DeleteTextures(1, &texture_);
        texture_ = 0;
        return;
    }
    // A window over everything takes the input while the screen shows, so nothing behind it is
    // clicked by accident.
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
    draw->AddRectFilled(lo, hi, ImGui::ColorConvertFloat4ToU32({paper_[0], paper_[1], paper_[2], alpha}));
    // The artwork at half its pixel size (sharp on Retina screens), smaller if the window is.
    const float aspect = static_cast<float>(height_) / static_cast<float>(width_);
    const float w = std::min({static_cast<float>(width_) / 4, size.x * 0.4F, size.y * 0.33F / aspect});
    const float h = w * aspect; // Half sizes.
    const ImVec2 centre{lo.x + size.x / 2, lo.y + size.y * 0.46F};
    draw->AddImage(static_cast<ImTextureID>(texture_), {centre.x - w, centre.y - h},
                   {centre.x + w, centre.y + h}, {0, 0}, {1, 1},
                   IM_COL32(255, 255, 255, static_cast<int>(alpha * 255)));
    // The version and what is happening, under the artwork.
    const auto faded = [&](ImU32 color, float a) {
        return (color & 0x00FFFFFF) | (static_cast<ImU32>(a * alpha * 255) << 24);
    };
    const std::string version = std::string("Version ") + editor_version;
    const auto vs = ImGui::CalcTextSize(version.c_str()), ss = ImGui::CalcTextSize(status.c_str());
    const float y = std::min(hi.y - 70, centre.y + h + 14);
    draw->AddText({centre.x - vs.x / 2, y}, faded(ink, 0.9F), version.c_str());
    draw->AddText({centre.x - ss.x / 2, y + vs.y + 6}, faded(ink, 0.55F), status.c_str());
    // A thin bar that fills while the screen shows.
    const float progress = static_cast<float>(std::min(1.0, (now - start_) / shown_seconds));
    const float bar = std::min(240.0F, w * 2 * 0.4F);
    const ImVec2 bar_lo{centre.x - bar / 2, y + vs.y + ss.y + 18};
    draw->AddRectFilled(bar_lo, {bar_lo.x + bar, bar_lo.y + 3}, faded(ink, 0.12F), 2);
    draw->AddRectFilled(bar_lo, {bar_lo.x + bar * progress, bar_lo.y + 3}, faded(ink, 0.8F), 2);
    ImGui::End();
}
} // namespace seed::editor
