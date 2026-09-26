#include "editor/editor.hpp"
#include "app/app.hpp"
#include <algorithm>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>

namespace seed {
Editor::Editor(Window& window, bool open) : open_(open) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr; // Panel layout is not persisted yet.
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    if (!ImGui_ImplSDL2_InitForOpenGL(window.handle(), window.context()) ||
        !ImGui_ImplOpenGL3_Init("#version 410 core")) {
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        throw std::runtime_error("Could not start the editor's ImGui backends");
    }
    window.observe(this, observe);
}

Editor::~Editor() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
}

void Editor::observe(void*, const SDL_Event& event) {
    // Always forwarded, so ImGui's key and mouse state is current when the editor opens.
    ImGui_ImplSDL2_ProcessEvent(&event);
}

void Editor::begin() {
    if (!open_) return;
    if (started_) cancel();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    started_ = true;
}

bool Editor::captures_mouse() const {
    return started_ && ImGui::GetIO().WantCaptureMouse;
}

bool Editor::captures_keyboard() const {
    return started_ && ImGui::GetIO().WantCaptureKeyboard;
}

void Editor::cancel() {
    if (!started_) return;
    ImGui::EndFrame();
    started_ = false;
}

void Editor::record(const Sample& sample) {
    last_ = sample;
    frame_ms_[frame_next_] = sample.seconds * 1000;
    frame_next_ = (frame_next_ + 1) % frame_ms_.size();
    frame_count_ = std::min(frame_count_ + 1, frame_ms_.size());
}

void Editor::finish(Engine& engine, const Game& game) {
    if (!started_) return;
    if (!open_) return cancel(); // Closed during this frame.
    stats(engine);
    if (game.editor) game.editor(game.context, engine);
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    started_ = false;
}

void Editor::stats(Engine& engine) {
    // Top-right, clear of the corner games usually put their HUD in.
    const auto& display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({display.x - 12, 12}, ImGuiCond_FirstUseEver, {1, 0});
    ImGui::SetNextWindowSize({320, 0}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Engine")) {
        ImGui::End();
        return;
    }
    float total = 0, worst = 0;
    for (std::size_t i = 0; i < frame_count_; ++i) {
        total += frame_ms_[i];
        worst = std::max(worst, frame_ms_[i]);
    }
    const float average = frame_count_ ? total / static_cast<float>(frame_count_) : 0;
    ImGui::Text("Frame %.2f ms avg, %.2f ms worst (%.0f fps)", average, worst,
                average > 0 ? 1000 / average : 0.0F);
    ImGui::PlotLines("##frames", frame_ms_.data(), static_cast<int>(frame_ms_.size()),
                     static_cast<int>(frame_next_), nullptr, 0, 33.3F, {-1, 48});
    ImGui::Text("Draw calls %u", engine.renderer.draw_calls());
    if (ImGui::CollapsingHeader("World", ImGuiTreeNodeFlags_DefaultOpen)) {
        unsigned chunks = 0;
        engine.world.each([&](ChunkCoord, const Chunk&) { ++chunks; });
        const auto focus = engine.focus_position();
        const auto generation = engine.world.generation_metrics();
        ImGui::Text("Seed %llu", static_cast<unsigned long long>(engine.options.seed));
        ImGui::Text("Active chunks %u", chunks);
        ImGui::Text("Focus chunk (%lld, %lld) at (%.2f, %.2f)", static_cast<long long>(focus.chunk.x),
                    static_cast<long long>(focus.chunk.y), focus.local.x, focus.local.y);
        ImGui::Text("Generated %llu chunks, %.2f ms worst",
                    static_cast<unsigned long long>(generation.chunks),
                    static_cast<double>(generation.maximum_nanoseconds) / 1e6);
    }
    if (ImGui::CollapsingHeader("Simulation", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Bodies %zu, step %.2f ms", last_.bodies,
                    static_cast<double>(last_.physics_nanoseconds) / 1e6);
        ImGui::Text("Entities %zu (%zu saved)", engine.scene.transforms.values().size(),
                    engine.scene.saved.values().size());
        ImGui::Text("Scene memory %.1f KiB", static_cast<double>(engine.scene.memory_used()) / 1024);
        ImGui::Text("Particles %zu", engine.particles.count());
    }
    if (ImGui::CollapsingHeader("Save", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextWrapped("%s", engine.options.save.string().c_str());
        ImGui::Text("%s", engine.checkpoint.saving() ? "Checkpoint in progress" : "Idle");
    }
    ImGui::End();
}
} // namespace seed
