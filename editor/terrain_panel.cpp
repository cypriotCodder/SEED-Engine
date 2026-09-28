// The Terrain panel of AssetPanels: world shape, noise fields and material rules.
#include "asset_panels.hpp"
#include "starter.hpp"
#include "widgets.hpp"
#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <limits>
#include <random>

namespace seed::editor {
namespace {
constexpr const char* kind_labels[] = {"Perlin", "Fractal", "Ridged", "Distance", "Spot"};
constexpr const char* kind_help[] = {
    "Smooth noise from -1 to 1 with features about a wavelength apart.",
    "Layered detail from wavelength 128 down to 8.",
    "1 - 2|noise|: sharp crests, for mountain ranges.",
    "Distance from the world centre: 0 at the centre, 1 at the rim.",
    "A bump at the centre, 1 there and falling off over the width, so spawn is on land.",
};
constexpr unsigned wavelengths[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024};

bool wavelength_combo(const char* id, unsigned& value) {
    bool changed = false;
    if (ImGui::BeginCombo(id, std::to_string(value).c_str())) {
        for (const unsigned w : wavelengths)
            if (ImGui::Selectable(std::to_string(w).c_str(), w == value)) {
                value = w;
                changed = true;
            }
        ImGui::EndCombo();
    }
    return changed;
}
bool material_combo(const char* id, std::string& value, const std::vector<MaterialAsset>& materials) {
    bool changed = false;
    if (ImGui::BeginCombo(id, value.empty() ? "(choose)" : value.c_str())) {
        for (const auto& m : materials)
            if (ImGui::Selectable(m.name.c_str(), m.name == value)) {
                value = m.name;
                changed = true;
            }
        ImGui::EndCombo();
    }
    return changed;
}
bool field_combo(const char* id, std::string& value, const std::vector<TerrainField>& fields) {
    bool changed = false;
    if (ImGui::BeginCombo(id, value.empty() ? "(choose)" : value.c_str())) {
        for (const auto& f : fields)
            if (ImGui::Selectable(f.name.c_str(), f.name == value)) {
                value = f.name;
                changed = true;
            }
        ImGui::EndCombo();
    }
    return changed;
}
// An optional bound: a checkbox turns it on, then a drag edits it.
bool bound(const char* id, float& value, float open) {
    bool changed = false, on = std::isfinite(value);
    ImGui::PushID(id);
    if (ImGui::Checkbox("##on", &on)) {
        value = on ? 0.0F : open;
        changed = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::BeginDisabled(!on);
    float shown = on ? value : 0;
    if (ImGui::DragFloat(id, &shown, 0.01F, -10, 10, on ? "%.2f" : "any") && on) {
        value = shown;
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::PopID();
    return changed;
}
} // namespace

void AssetPanels::terrain() {
    // The panel edits the terrain of the scene being edited (see set_terrain).
    const auto found = edited_.terrains.find(terrain_name_);
    const bool edited =
        found == edited_.terrains.end()
            ? saved_.terrains.count(terrain_name_) > 0
            : !saved_.terrains.count(terrain_name_) || saved_.terrains.at(terrain_name_) != found->second;
    const auto title = "Terrain: " + terrain_name_ + (edited ? " *" : "") + "###Terrain";
    if (!ImGui::Begin(title.c_str(), &show_terrain)) {
        ImGui::End();
        return;
    }
    if (found == edited_.terrains.end()) {
        ImGui::TextWrapped(
            "This scene uses the terrain \"%s\", which does not exist yet, so its ground is flat. "
            "Create it, or pick another terrain by selecting Terrain in the Hierarchy.",
            terrain_name_.c_str());
        if (ImGui::Button("Create Starter Island", {-1, 32})) {
            starter_island(edited_, terrain_name_);
            refresh();
        }
        if (ImGui::Button("Create Blank Terrain", {-1, 0})) {
            edited_.terrains[terrain_name_] = {};
            refresh();
        }
        ImGui::End();
        return;
    }
    auto& t = found->second;
    problems("Terrain");
    const auto before = t;
    const auto before_materials = edited_.materials;

    if (!t.enabled()) {
        ImGui::TextWrapped("This terrain has no rules yet: every tile is the first material. Start from the "
                           "starter island, or add fields and rules yourself.");
        if (ImGui::Button("Make It a Starter Island", {-1, 32})) {
            starter_island(edited_, terrain_name_);
            refresh();
            ImGui::End();
            return;
        }
        ImGui::Separator();
    }
    // World shape and preview seed.
    ImGui::SetNextItemWidth(160);
    ImGui::InputScalar("Seed", ImGuiDataType_U64, &t.default_seed);
    ImGui::SetItemTooltip("The world the Scene view shows, and the game's default seed.");
    ImGui::SameLine();
    if (ImGui::Button("Randomize")) t.default_seed = std::random_device{}() & 0xffffffffU;
    ImGui::Checkbox("Island", &t.island);
    ImGui::SetItemTooltip("A disc of land in endless ocean. Off: the world goes on forever.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170);
    ImGui::SliderFloat("Radius (chunks)", &t.radius, 4, 2000, "%.0f", ImGuiSliderFlags_Logarithmic);
    if (t.island) {
        ImGui::SetNextItemWidth(170);
        ImGui::SliderFloat("Coast width", &t.coast, 0, 0.5F);
    }
    ImGui::SetNextItemWidth(170);
    ImGui::SliderFloat("Warp (tiles)", &t.warp, 0, 128, "%.0f");
    ImGui::SetItemTooltip("Bends coastlines and borders so they look less like smooth blobs.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    wavelength_combo("Warp wavelength", t.warp_wavelength);

    if (ImGui::BeginTabBar("terrain tabs")) {
        if (ImGui::BeginTabItem("Rules")) {
            ImGui::TextDisabled("Each tile takes the first rule whose ranges all hold.");
            if (ImGui::BeginTable("rules", 2, ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupColumn("list", ImGuiTableColumnFlags_WidthFixed, 150);
                ImGui::TableNextColumn();
                list(t.rules, rule_, "rule", TerrainAsset::rule_capacity, "Earlier rules win.");
                ImGui::TableNextColumn();
                if (rule_ >= 0 && rule_ < static_cast<int>(t.rules.size())) {
                    auto& r = t.rules[static_cast<std::size_t>(rule_)];
                    ImGui::SetNextItemWidth(-1);
                    ImGui::InputText("##name", &r.name);
                    ImGui::TextUnformatted("Material");
                    ImGui::SameLine(110);
                    ImGui::SetNextItemWidth(-1);
                    material_combo("##material", r.material, edited_.materials);
                    ImGui::Checkbox("Solid", &r.solid);
                    ImGui::SetItemTooltip("Solid tiles block movement.");
                    ImGui::SeparatorText("When");
                    for (std::size_t i = 0; i < r.when.size(); ++i) {
                        auto& c = r.when[i];
                        ImGui::PushID(static_cast<int>(i));
                        if (ImGui::SmallButton("x")) {
                            r.when.erase(r.when.begin() + static_cast<std::ptrdiff_t>(i));
                            ImGui::PopID();
                            break;
                        }
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(110);
                        field_combo("##field", c.field, t.fields);
                        ImGui::SameLine();
                        ImGui::TextUnformatted("from");
                        ImGui::SameLine();
                        bound("##min", c.min, -std::numeric_limits<float>::infinity());
                        ImGui::SameLine();
                        ImGui::TextUnformatted("to");
                        ImGui::SameLine();
                        bound("##max", c.max, std::numeric_limits<float>::infinity());
                        ImGui::PopID();
                    }
                    if (r.when.empty()) ImGui::TextDisabled("Always (a fallback).");
                    ImGui::BeginDisabled(t.fields.empty());
                    if (ImGui::SmallButton("Add range")) r.when.push_back({t.fields.front().name});
                    ImGui::EndDisabled();
                    ImGui::SeparatorText("Scatter");
                    bool scatter = r.scatter.has_value();
                    if (ImGui::Checkbox("Scatter objects", &scatter))
                        r.scatter = scatter ? std::optional<TerrainScatter>(TerrainScatter{}) : std::nullopt;
                    ImGui::SetItemTooltip("Place an object (a tree, a rock) on some of this rule's tiles.");
                    if (r.scatter) {
                        ImGui::TextUnformatted("Material");
                        ImGui::SameLine(110);
                        ImGui::SetNextItemWidth(-1);
                        material_combo("##scatter", r.scatter->material, edited_.materials);
                        int one_in = static_cast<int>(r.scatter->one_in);
                        ImGui::TextUnformatted("One tile in");
                        ImGui::SameLine(110);
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::DragInt("##one_in", &one_in, 0.2F, 1, 100000))
                            r.scatter->one_in = static_cast<std::uint32_t>(std::max(1, one_in));
                        ImGui::Checkbox("Objects are solid", &r.scatter->solid);
                    }
                } else
                    ImGui::TextDisabled("Select a rule.");
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Fields")) {
            ImGui::TextDisabled("Each field is its base plus the sum of its terms, per tile.");
            if (ImGui::BeginTable("fields", 2, ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupColumn("list", ImGuiTableColumnFlags_WidthFixed, 150);
                ImGui::TableNextColumn();
                list(t.fields, field_, "field", TerrainAsset::field_capacity, nullptr);
                ImGui::TableNextColumn();
                if (field_ >= 0 && field_ < static_cast<int>(t.fields.size())) {
                    auto& f = t.fields[static_cast<std::size_t>(field_)];
                    ImGui::SetNextItemWidth(-1);
                    ImGui::InputText("##name", &f.name);
                    if (f.name == "elevation")
                        ImGui::TextDisabled("Elevation is the tile height: below 0 is water%s.",
                                            t.island ? "; the coast pulls it down near the rim" : "");
                    ImGui::SetNextItemWidth(120);
                    ImGui::DragFloat("Base", &f.base, 0.01F);
                    if (ImGui::BeginTable("terms", 5,
                                          ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
                        ImGui::TableSetupColumn("Type");
                        ImGui::TableSetupColumn("Wavelength");
                        ImGui::TableSetupColumn("Amount");
                        ImGui::TableSetupColumn("Warp");
                        ImGui::TableSetupColumn("");
                        ImGui::TableHeadersRow();
                        for (std::size_t i = 0; i < f.terms.size(); ++i) {
                            auto& term = f.terms[i];
                            ImGui::PushID(static_cast<int>(i));
                            ImGui::TableNextColumn();
                            int kind = static_cast<int>(term.kind);
                            ImGui::SetNextItemWidth(90);
                            if (ImGui::Combo("##kind", &kind, kind_labels, 5)) {
                                term.kind = static_cast<TerrainTerm::Kind>(kind);
                                if (term.kind == TerrainTerm::Kind::spot) term.wavelength = 40;
                            }
                            ImGui::SetItemTooltip("%s", kind_help[kind]);
                            ImGui::TableNextColumn();
                            ImGui::SetNextItemWidth(80);
                            if (term.kind == TerrainTerm::Kind::perlin ||
                                term.kind == TerrainTerm::Kind::ridged)
                                wavelength_combo("##wavelength", term.wavelength);
                            else if (term.kind == TerrainTerm::Kind::spot) {
                                int width = static_cast<int>(term.wavelength);
                                if (ImGui::DragInt("##width", &width, 1, 1, 1024))
                                    term.wavelength = static_cast<unsigned>(std::clamp(width, 1, 1024));
                            } else
                                ImGui::TextDisabled("-");
                            ImGui::TableNextColumn();
                            ImGui::SetNextItemWidth(80);
                            ImGui::DragFloat("##amplitude", &term.amplitude, 0.01F, -100, 100, "%.2f");
                            ImGui::TableNextColumn();
                            const bool noise = term.kind != TerrainTerm::Kind::distance &&
                                               term.kind != TerrainTerm::Kind::spot;
                            if (noise) ImGui::Checkbox("##warped", &term.warped);
                            ImGui::TableNextColumn();
                            const bool remove = ImGui::SmallButton("x");
                            ImGui::PopID();
                            if (remove) {
                                f.terms.erase(f.terms.begin() + static_cast<std::ptrdiff_t>(i));
                                break;
                            }
                        }
                        ImGui::EndTable();
                    }
                    ImGui::BeginDisabled(f.terms.size() >= TerrainAsset::term_capacity);
                    if (ImGui::SmallButton("Add term")) f.terms.push_back({});
                    ImGui::EndDisabled();
                } else
                    ImGui::TextDisabled("Select a field.");
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (t != before || edited_.materials != before_materials) refresh();
    ImGui::End();
}
} // namespace seed::editor
