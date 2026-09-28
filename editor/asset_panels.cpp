#include "asset_panels.hpp"
#include "widgets.hpp"
#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <imgui_stdlib.h>

namespace seed::editor {
namespace {
constexpr const char* pattern_labels[] = {"Speckle", "Water", "Planks", "Round"};

// Begins a panel window, marking unsaved edits with '*'; false when the window is collapsed. The
// "###" suffix keeps the window's identity (and docked position) while its title changes.
bool begin_panel(const char* name, bool edited, bool* open) {
    const auto title = std::string(name) + (edited ? " *" : "") + "###" + name;
    if (!ImGui::Begin(title.c_str(), open)) {
        ImGui::End();
        return false;
    }
    return true;
}

bool field(const char* label) {
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1);
    return true;
}
} // namespace

AssetPanels::AssetPanels(Log log, bool audio) : log_(std::move(log)), audio_(audio, Sounds{}) {}

void AssetPanels::load(const std::filesystem::path& folder) {
    folder_ = folder;
    saved_ = load_assets(folder);
    edited_ = saved_;
    const auto first = [](const auto& list) {
        return list.empty() ? -1 : 0;
    };
    material_ = first(edited_.materials);
    action_ = first(edited_.actions);
    sound_ = first(edited_.sounds);
    particle_ = first(edited_.particles);
    // The Terrain panel keeps its row selections in range for whichever terrain it shows.
    rule_ = field_ = 0;
    capturing_ = -1;
    refresh();
    if (!problems_.empty()) log_(true, "The project's assets have problems:\n" + problems_);
}

void AssetPanels::set(const Assets& assets) {
    edited_ = assets;
    const auto clamp = [](int& selected, const auto& list) {
        selected = std::min(selected, static_cast<int>(list.size()) - 1);
        if (selected < 0 && !list.empty()) selected = 0;
    };
    clamp(material_, edited_.materials);
    clamp(action_, edited_.actions);
    clamp(sound_, edited_.sounds);
    clamp(particle_, edited_.particles);
    capturing_ = -1;
    refresh();
}

void AssetPanels::unload() {
    folder_.clear();
    saved_ = edited_ = {};
    problems_.clear();
}

bool AssetPanels::save() {
    if (!dirty()) return true;
    if (!problems_.empty()) {
        log_(true, "Assets not saved. Fix these first:\n" + problems_);
        return false;
    }
    save_assets(folder_, edited_, &saved_);
    saved_ = edited_;
    log_(false, "Saved assets.");
    return true;
}

bool AssetPanels::capture(const SDL_Event& event) {
    if (capturing_ < 0 || capturing_ >= static_cast<int>(edited_.actions.size())) return false;
    auto& bindings = edited_.actions[static_cast<std::size_t>(capturing_)].bindings;
    if (event.type == SDL_KEYDOWN && !event.key.repeat) {
        if (event.key.keysym.scancode != SDL_SCANCODE_ESCAPE)
            bindings.push_back(Binding::key(event.key.keysym.scancode));
    } else if (event.type == SDL_MOUSEBUTTONDOWN)
        bindings.push_back(Binding::mouse(event.button.button));
    else
        return false;
    capturing_ = -1;
    refresh();
    return true;
}

void AssetPanels::draw() {
    if (folder_.empty()) return;
    if (show_materials) materials();
    if (show_input) input();
    if (show_sounds) sounds();
    if (show_particles) particles();
    if (show_terrain) terrain();
}

void AssetPanels::problems(const char* kind) {
    // Show only this panel's lines; each starts with its kind.
    std::size_t start = 0;
    bool any = false;
    while (start < problems_.size()) {
        const auto end = problems_.find('\n', start);
        const auto line = problems_.substr(start, end - start);
        if (line.rfind(kind, 0) == 0) {
            any = true;
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored({0.95F, 0.55F, 0.4F, 1}, "%s", line.c_str());
            ImGui::PopTextWrapPos();
        }
        start = end + 1;
    }
    if (any) ImGui::Separator();
}

void AssetPanels::materials() {
    if (!begin_panel("Materials", edited_.materials != saved_.materials, &show_materials)) return;
    problems("Material");
    bool changed = false;
    if (ImGui::BeginTable("layout", 2, ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("list", ImGuiTableColumnFlags_WidthFixed, 170);
        ImGui::TableNextColumn();
        changed |=
            list(edited_.materials, material_, "material", Materials::capacity,
                 "Tiles store a material's position in this list, so moving one changes what saved worlds "
                 "show.");
        ImGui::TableNextColumn();
        if (material_ >= 0) {
            auto& m = edited_.materials[static_cast<std::size_t>(material_)];
            field("Name");
            changed |= ImGui::InputText("##name", &m.name);
            float color[3] = {m.color[0] / 255.0F, m.color[1] / 255.0F, m.color[2] / 255.0F};
            field("Color");
            if (ImGui::ColorEdit3("##color", color)) {
                for (int i = 0; i < 3; ++i)
                    m.color[static_cast<std::size_t>(i)] = static_cast<int>(std::lround(color[i] * 255));
                changed = true;
            }
            int pattern = static_cast<int>(m.pattern);
            field("Pattern");
            if (ImGui::Combo("##pattern", &pattern, pattern_labels, 4)) {
                m.pattern = static_cast<Pattern>(pattern);
                changed = true;
            }
            field("Variation");
            changed |= ImGui::SliderInt("##variation", &m.variation, 1, 128);
            ImGui::SetItemTooltip("How much each pixel's brightness varies.");
            field("Texture (optional)");
            changed |= ImGui::InputTextWithHint("##texture", "Generated from the settings above", &m.texture);
            ImGui::TextDisabled("ID %d", material_);
        } else
            ImGui::TextDisabled("Select a material.");
        ImGui::EndTable();
    }
    if (changed) refresh();
    ImGui::End();
}

void AssetPanels::input() {
    if (!begin_panel("Input", edited_.actions != saved_.actions, &show_input)) return;
    problems("Action");
    bool changed = false;
    if (ImGui::BeginTable("layout", 2, ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("list", ImGuiTableColumnFlags_WidthFixed, 170);
        ImGui::TableNextColumn();
        if (list(edited_.actions, action_, "action", Actions::capacity - 3, nullptr)) {
            changed = true;
            capturing_ = -1;
        }
        ImGui::TableNextColumn();
        if (action_ >= 0) {
            auto& a = edited_.actions[static_cast<std::size_t>(action_)];
            field("Name");
            changed |= ImGui::InputText("##name", &a.name);
            ImGui::SetItemTooltip("Scripts read the action by this name.");
            ImGui::TextUnformatted("Bindings");
            for (std::size_t i = 0; i < a.bindings.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::SmallButton("x")) {
                    a.bindings.erase(a.bindings.begin() + static_cast<std::ptrdiff_t>(i));
                    changed = true;
                    ImGui::PopID();
                    break;
                }
                ImGui::SameLine();
                ImGui::TextUnformatted(binding_name(a.bindings[i]).c_str());
                ImGui::PopID();
            }
            if (capturing_ == action_)
                ImGui::TextColored({0.45F, 0.8F, 0.62F, 1}, "Press a key or mouse button (Esc cancels)...");
            else {
                ImGui::BeginDisabled(a.bindings.size() >= Actions::bindings_per_action);
                if (ImGui::Button("Add binding")) capturing_ = action_;
                ImGui::EndDisabled();
            }
        } else
            ImGui::TextDisabled("Select an action.");
        ImGui::EndTable();
    }
    ImGui::TextDisabled("The engine reserves quit (Esc), checkpoint (F5) and screenshot (F12).");
    if (changed) refresh();
    ImGui::End();
}

void AssetPanels::sounds() {
    if (!begin_panel("Sounds", edited_.sounds != saved_.sounds, &show_sounds)) return;
    problems("Sound");
    bool changed = false;
    if (ImGui::BeginTable("layout", 2, ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("list", ImGuiTableColumnFlags_WidthFixed, 170);
        ImGui::TableNextColumn();
        changed |= list(edited_.sounds, sound_, "sound", Sounds::capacity, nullptr);
        ImGui::TableNextColumn();
        if (sound_ >= 0) {
            auto& s = edited_.sounds[static_cast<std::size_t>(sound_)];
            field("Name");
            changed |= ImGui::InputText("##name", &s.name);
            field("Pitch (Hz)");
            changed |= ImGui::SliderFloat("##frequency", &s.frequency, 20, 4000, "%.0f",
                                          ImGuiSliderFlags_Logarithmic);
            field("Pitch variation (Hz)");
            changed |= ImGui::SliderFloat("##variation", &s.variation, 0, 1000, "%.0f");
            ImGui::SetItemTooltip("Repeated plays step through this range so they don't sound identical.");
            field("Volume");
            changed |= ImGui::SliderFloat("##gain", &s.gain, 0, 1);
            // Shown as a fade time: the decay per sample is hard to reason about directly.
            float fade = std::log(0.001F) / std::log(s.decay) / 48000;
            field("Fade time (s)");
            if (ImGui::SliderFloat("##fade", &fade, 0.01F, 5, "%.2f", ImGuiSliderFlags_Logarithmic)) {
                s.decay = std::pow(0.001F, 1 / (fade * 48000));
                changed = true;
            }
            field("Tone");
            changed |= ImGui::SliderFloat("##tone", &s.tone, 0, 1);
            ImGui::SetItemTooltip("0 is pure noise, 1 a pure tone.");
            ImGui::BeginDisabled(!problems_.empty() &&
                                 problems_.find("Sound \"" + s.name + "\"") != std::string::npos);
            if (ImGui::Button("Play", {100, 0})) audio_.play(s.desc());
            ImGui::EndDisabled();
        } else
            ImGui::TextDisabled("Select a sound.");
        ImGui::EndTable();
    }
    if (changed) refresh();
    ImGui::End();
}

void AssetPanels::particles() {
    if (!begin_panel("Particles", edited_.particles != saved_.particles, &show_particles)) return;
    problems("Particle style");
    bool changed = false;
    if (ImGui::BeginTable("layout", 2, ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("list", ImGuiTableColumnFlags_WidthFixed, 170);
        ImGui::TableNextColumn();
        changed |= list(edited_.particles, particle_, "particles", Particles::style_capacity, nullptr);
        ImGui::TableNextColumn();
        if (particle_ >= 0) {
            auto& p = edited_.particles[static_cast<std::size_t>(particle_)];
            field("Name");
            changed |= ImGui::InputText("##name", &p.name);
            field("Material");
            if (ImGui::BeginCombo("##material", p.material.empty() ? "(choose)" : p.material.c_str())) {
                for (const auto& m : edited_.materials)
                    if (ImGui::Selectable(m.name.c_str(), m.name == p.material)) {
                        p.material = m.name;
                        changed = true;
                    }
                ImGui::EndCombo();
            }
            int count = static_cast<int>(p.count);
            field("Particles per burst");
            if (ImGui::SliderInt("##count", &count, 1, 512)) {
                p.count = static_cast<unsigned>(count);
                changed = true;
            }
            field("Slowest speed (units/s)");
            changed |= ImGui::SliderFloat("##speed", &p.speed, 0, 20);
            field("Speed range (units/s)");
            changed |= ImGui::SliderFloat("##range", &p.speed_range, 0, 20);
            ImGui::SetItemTooltip("Launch speeds are spread between the slowest speed and slowest + range.");
            field("Life (s)");
            changed |= ImGui::SliderFloat("##life", &p.life, 0.05F, 5);
            field("Size (units)");
            changed |= ImGui::SliderFloat("##size", &p.size, 0.01F, 2);
            field("Drag");
            changed |= ImGui::SliderFloat("##drag", &p.drag, 0, 1);
            ImGui::SetItemTooltip("Speed kept each step: 1 never slows, 0 stops at once.");
            field("Spin (rad/s)");
            changed |= ImGui::SliderFloat("##spin", &p.spin, -20, 20);
            field("Brightness");
            changed |= ImGui::SliderFloat("##shade", &p.shade, 0, 4);
        } else
            ImGui::TextDisabled("Select a particle style.");
        ImGui::EndTable();
    }
    if (changed) refresh();
    ImGui::End();
}
} // namespace seed::editor
