#include "project/terrain.hpp"
#include "world/noise.hpp"
#include <algorithm>
#include <cmath>

namespace seed {
namespace {
constexpr const char* kind_names[] = {"perlin", "fractal", "ridged", "distance", "spot"};
constexpr std::uint64_t warp_x_stream = 0x6a09e667f3bcc908ULL, warp_y_stream = 0xbb67ae8584caa73bULL;
constexpr std::uint64_t scatter_stream = 0x3c6ef372fe94f82bULL;

bool power_of_two(unsigned n) {
    return n >= 1 && n <= 1024 && !(n & (n - 1));
}
float smoothstep(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0F, 1.0F);
    return t * t * (3 - 2 * t);
}
std::uint32_t fnv32(std::string_view text) {
    std::uint32_t hash = 2166136261U;
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 16777619U;
    }
    return hash ? hash : 1; // Zero is not a valid generator version.
}
TerrainTerm::Kind parse_kind(const std::string& name) {
    for (std::size_t i = 0; i < std::size(kind_names); ++i)
        if (name == kind_names[i]) return static_cast<TerrainTerm::Kind>(i);
    throw std::runtime_error("Unknown term type \"" + name + "\"");
}
float get(const Json& object, std::string_view key, float fallback) {
    const auto* value = object.find(key);
    return value ? static_cast<float>(value->as_number()) : fallback;
}
} // namespace

std::string TerrainAsset::problems(const std::vector<std::string>& materials) const {
    std::string out;
    const auto known = [&](const std::string& name) {
        return std::find(materials.begin(), materials.end(), name) != materials.end();
    };
    const auto field_known = [&](const std::string& name) {
        return std::any_of(fields.begin(), fields.end(),
                           [&](const TerrainField& f) { return f.name == name; });
    };
    if (!(radius >= 1 && radius <= 100000)) out += "Radius must be 1 to 100000 chunks\n";
    if (!(coast >= 0 && coast <= 1)) out += "Coast must be 0 to 1\n";
    if (!(warp >= 0 && warp <= 256)) out += "Warp must be 0 to 256 tiles\n";
    if (!power_of_two(warp_wavelength)) out += "Warp wavelength must be a power of two up to 1024\n";
    if (fields.size() > field_capacity) out += "At most 8 fields\n";
    if (rules.size() > rule_capacity) out += "At most 64 rules\n";
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto& f = fields[i];
        const auto where = "Field \"" + f.name + "\": ";
        if (f.name.empty() || f.name.size() > 32) out += where + "names need 1 to 32 characters\n";
        for (std::size_t j = 0; j < i; ++j)
            if (fields[j].name == f.name) out += where + "duplicate name\n";
        if (!std::isfinite(f.base)) out += where + "base must be finite\n";
        if (f.terms.size() > term_capacity) out += where + "at most 16 terms\n";
        for (const auto& t : f.terms) {
            const bool shaped = t.kind == TerrainTerm::Kind::perlin || t.kind == TerrainTerm::Kind::ridged;
            if (shaped && !power_of_two(t.wavelength))
                out += where + "noise wavelengths must be powers of two up to 1024\n";
            if (t.kind == TerrainTerm::Kind::spot && !(t.wavelength >= 1 && t.wavelength <= 1024))
                out += where + "spot width must be 1 to 1024\n";
            if (!std::isfinite(t.amplitude) || std::abs(t.amplitude) > 100)
                out += where + "amplitude must be within ±100\n";
        }
    }
    if (enabled() && !field_known("elevation")) out += "A field named \"elevation\" is required\n";
    for (const auto& r : rules) {
        const auto where = "Rule \"" + r.name + "\": ";
        if (!known(r.material)) out += where + "unknown material \"" + r.material + "\"\n";
        for (const auto& c : r.when) {
            if (!field_known(c.field)) out += where + "unknown field \"" + c.field + "\"\n";
            if (std::isnan(c.min) || std::isnan(c.max) || c.min > c.max)
                out += where + "a range's minimum exceeds its maximum\n";
        }
        if (r.scatter) {
            if (!known(r.scatter->material))
                out += where + "unknown scatter material \"" + r.scatter->material + "\"\n";
            if (!r.scatter->one_in) out += where + "scatter chance must be 1 in 1 or rarer\n";
        }
    }
    return out;
}

Json terrain_json(const TerrainAsset& t) {
    auto out = Json::object();
    out.set("island", t.island);
    out.set("radius", json_float(t.radius));
    out.set("coast", json_float(t.coast));
    out.set("warp", json_float(t.warp));
    out.set("warp_wavelength", static_cast<int>(t.warp_wavelength));
    out.set("default_seed", static_cast<std::int64_t>(t.default_seed & ((std::uint64_t(1) << 53) - 1)));
    auto fields = Json::array();
    for (const auto& f : t.fields) {
        auto field = Json::object();
        field.set("name", f.name);
        field.set("base", json_float(f.base));
        auto terms = Json::array();
        for (const auto& term : f.terms) {
            auto entry = Json::object();
            entry.set("type", kind_names[static_cast<int>(term.kind)]);
            entry.set("wavelength", static_cast<int>(term.wavelength));
            entry.set("amplitude", json_float(term.amplitude));
            entry.set("warped", term.warped);
            terms.push(entry);
        }
        field.set("terms", terms);
        fields.push(field);
    }
    out.set("fields", fields);
    auto rules = Json::array();
    for (const auto& r : t.rules) {
        auto rule = Json::object();
        rule.set("name", r.name);
        rule.set("material", r.material);
        auto when = Json::array();
        for (const auto& c : r.when) {
            auto condition = Json::object();
            condition.set("field", c.field);
            // Open ends are left out: JSON has no infinity.
            if (std::isfinite(c.min)) condition.set("min", json_float(c.min));
            if (std::isfinite(c.max)) condition.set("max", json_float(c.max));
            when.push(condition);
        }
        rule.set("when", when);
        rule.set("solid", r.solid);
        if (r.scatter) {
            auto scatter = Json::object();
            scatter.set("material", r.scatter->material);
            scatter.set("one_in", static_cast<std::int64_t>(r.scatter->one_in));
            scatter.set("solid", r.scatter->solid);
            rule.set("scatter", scatter);
        }
        rules.push(rule);
    }
    out.set("rules", rules);
    return out;
}

TerrainAsset parse_terrain(const Json& json) {
    TerrainAsset t;
    if (const auto* island = json.find("island")) t.island = island->as_bool();
    t.radius = get(json, "radius", t.radius);
    t.coast = get(json, "coast", t.coast);
    t.warp = get(json, "warp", t.warp);
    if (const auto* w = json.find("warp_wavelength"))
        t.warp_wavelength = static_cast<unsigned>(w->as_int(1, 1024));
    if (const auto* s = json.find("default_seed"))
        t.default_seed = static_cast<std::uint64_t>(s->as_int(0, (std::int64_t(1) << 53) - 1));
    if (const auto* fields = json.find("fields"))
        for (const auto& item : fields->items()) {
            TerrainField f;
            f.name = item.at("name").as_string();
            f.base = get(item, "base", 0);
            if (const auto* terms = item.find("terms"))
                for (const auto& entry : terms->items()) {
                    TerrainTerm term;
                    term.kind = parse_kind(entry.at("type").as_string());
                    if (const auto* w = entry.find("wavelength"))
                        term.wavelength = static_cast<unsigned>(w->as_int(1, 1024));
                    term.amplitude = get(entry, "amplitude", 1);
                    if (const auto* warped = entry.find("warped")) term.warped = warped->as_bool();
                    f.terms.push_back(term);
                }
            t.fields.push_back(std::move(f));
        }
    if (const auto* rules = json.find("rules"))
        for (const auto& item : rules->items()) {
            TerrainRule r;
            r.name = item.at("name").as_string();
            r.material = item.at("material").as_string();
            if (const auto* when = item.find("when"))
                for (const auto& entry : when->items()) {
                    TerrainCondition c;
                    c.field = entry.at("field").as_string();
                    c.min = get(entry, "min", c.min);
                    c.max = get(entry, "max", c.max);
                    r.when.push_back(c);
                }
            if (const auto* solid = item.find("solid")) r.solid = solid->as_bool();
            if (const auto* scatter = item.find("scatter")) {
                TerrainScatter s;
                s.material = scatter->at("material").as_string();
                s.one_in = static_cast<std::uint32_t>(scatter->at("one_in").as_int(1, 1000000));
                if (const auto* solid = scatter->find("solid")) s.solid = solid->as_bool();
                r.scatter = s;
            }
            t.rules.push_back(std::move(r));
        }
    return t;
}

Terrain::Terrain(const TerrainAsset& asset, const Materials& materials, const TerrainPaint& paint)
    : asset_(asset) {
    std::vector<std::string> names;
    for (std::size_t i = 0; i < materials.size(); ++i)
        names.emplace_back(materials[static_cast<MaterialId>(i)].name);
    if (const auto problems = asset.problems(names); !problems.empty())
        throw std::invalid_argument("Terrain has problems:\n" + problems);
    const auto field_index = [&](const std::string& name) {
        return static_cast<std::size_t>(std::find_if(asset.fields.begin(), asset.fields.end(),
                                                     [&](const TerrainField& f) { return f.name == name; }) -
                                        asset.fields.begin());
    };
    for (const auto& f : asset.fields) {
        Field field{f.base, {}};
        // Each term draws from its own noise stream, derived from the field's name and position.
        for (std::size_t i = 0; i < f.terms.size(); ++i)
            field.terms.push_back({f.terms[i], mix64(stable_id(f.name) + i * 0x9e3779b97f4a7c15ULL)});
        fields_.push_back(std::move(field));
    }
    elevation_ = field_index("elevation");
    for (const auto& r : asset.rules) {
        Rule rule{materials.find(r.material), r.solid, {}, r.scatter.has_value(), 0, 1, false};
        for (const auto& c : r.when)
            rule.when.push_back({field_index(c.field), c.min, c.max});
        if (r.scatter) {
            rule.scatter_material = materials.find(r.scatter->material);
            rule.one_in = r.scatter->one_in;
            rule.scatter_solid = r.scatter->solid;
        }
        rules_.push_back(std::move(rule));
    }
    auto identity = asset;
    identity.default_seed = 0; // The seed chooses a world; it does not change the generator.
    auto identity_text = to_json(terrain_json(identity));
    if (!paint.empty()) {
        if (const auto problems = paint.problems(names); !problems.empty())
            throw std::invalid_argument("Terrain paint has problems:\n" + problems);
        std::vector<MaterialId> ids;
        for (const auto& name : paint.materials) {
            const bool known = std::find(names.begin(), names.end(), name) != names.end();
            ids.push_back(known ? materials.find(name) : MaterialId{}); // Unknown ones are unused.
        }
        for (const auto& [key, tiles] : paint.chunks) {
            auto& out = paint_[key];
            for (const auto& [index, t] : tiles)
                out.push_back(
                    {index, t.mask, (t.mask & paint_ground) ? ids[t.ground] : MaterialId{},
                     (t.mask & paint_object) && t.object ? tile_object(ids[t.object - 1u]) : no_object,
                     t.solid, t.elevation});
        }
        identity_text += encode_paint(paint); // Paint is part of what this terrain generates.
    }
    version_ = fnv32(identity_text);
}

Terrain::Sample Terrain::sample(std::uint64_t seed, WorldPosition position) const {
    position.move({});
    Sample out;
    if (rules_.empty()) return out;
    const double wx = static_cast<double>(position.chunk.x) * chunk_side + position.local.x;
    const double wy = static_cast<double>(position.chunk.y) * chunk_side + position.local.y;
    const double tiles = std::sqrt(wx * wx + wy * wy);
    const float distance = static_cast<float>(tiles / (static_cast<double>(asset_.radius) * chunk_side));
    // Well past an island's rim there is only ocean: skip the noise and keep huge coordinates out
    // of float math.
    const bool open_sea = asset_.island && distance > 1.1F;
    auto warped = position;
    if (asset_.warp > 0 && !open_sea)
        warped.move(Vec2{perlin(seed ^ warp_x_stream, position, asset_.warp_wavelength),
                         perlin(seed ^ warp_y_stream, position, asset_.warp_wavelength)} *
                    asset_.warp);
    std::array<float, TerrainAsset::field_capacity> values{};
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        float value = fields_[i].base;
        for (const auto& [term, stream] : fields_[i].terms) {
            const auto& at = term.warped ? warped : position;
            float v = 0;
            switch (term.kind) {
            case TerrainTerm::Kind::perlin:
                v = open_sea ? 0 : perlin(seed ^ stream, at, term.wavelength);
                break;
            case TerrainTerm::Kind::fractal:
                v = open_sea ? 0 : fractal(seed ^ stream, at);
                break;
            case TerrainTerm::Kind::ridged:
                v = open_sea ? 0 : 1 - 2 * std::abs(perlin(seed ^ stream, at, term.wavelength));
                break;
            case TerrainTerm::Kind::distance:
                v = distance;
                break;
            case TerrainTerm::Kind::spot: {
                const float r = static_cast<float>(tiles) / static_cast<float>(term.wavelength);
                v = std::exp(-r * r);
                break;
            }
            }
            value += term.amplitude * v;
        }
        values[i] = value;
    }
    if (asset_.island)
        values[elevation_] =
            open_sea ? -1.0F : values[elevation_] - smoothstep(1 - asset_.coast, 1, distance) * 1.6F;
    out.elevation = values[elevation_];

    const Rule* chosen = &rules_.back();
    bool matched = false;
    for (const auto& rule : rules_) {
        if (std::all_of(rule.when.begin(), rule.when.end(), [&](const Condition& c) {
                return values[c.field] >= c.min && values[c.field] <= c.max;
            })) {
            chosen = &rule;
            matched = true;
            break;
        }
    }
    out.material = chosen->material;
    out.solid = matched && chosen->solid;
    if (matched && chosen->scatter) {
        const auto x = static_cast<std::uint64_t>(position.chunk.x) * chunk_side +
                       static_cast<std::uint64_t>(position.local.x);
        const auto y = static_cast<std::uint64_t>(position.chunk.y) * chunk_side +
                       static_cast<std::uint64_t>(position.local.y);
        if (world_hash(seed ^ scatter_stream, x, y) % chosen->one_in == 0) {
            out.object = tile_object(chosen->scatter_material);
            out.solid = out.solid || chosen->scatter_solid;
        }
    }
    return out;
}

void Terrain::fill(void* context, std::uint64_t seed, ChunkCoord coord, Chunk& chunk) {
    const auto& terrain = *static_cast<const Terrain*>(context);
    for (int y = 0; y < chunk_side; ++y)
        for (int x = 0; x < chunk_side; ++x) {
            const auto s =
                terrain.sample(seed, {coord, {static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F}});
            auto& tile = chunk.tiles[static_cast<std::size_t>(y * chunk_side + x)];
            tile.elevation = s.elevation;
            tile.material = s.material;
            tile.object = s.object;
            tile.flags = s.solid ? tile_solid : 0;
        }
    const auto painted = terrain.paint_.find({coord.x, coord.y});
    if (painted == terrain.paint_.end()) return;
    for (const auto& p : painted->second) {
        auto& tile = chunk.tiles[p.index];
        if (p.mask & paint_ground) tile.material = p.ground;
        if (p.mask & paint_object) tile.object = p.object;
        if (p.mask & paint_height) tile.elevation = p.elevation;
        if (p.mask & paint_solid)
            tile.flags =
                static_cast<std::uint8_t>(p.solid ? tile.flags | tile_solid : tile.flags & ~tile_solid);
    }
}

WorldGenerator Terrain::generator() const {
    WorldGenerator generator;
    generator.context = const_cast<Terrain*>(this); // fill() only reads through it.
    generator.name = "seed-terrain";
    generator.version = version_;
    generator.terrain = fill;
    return generator;
}
} // namespace seed
