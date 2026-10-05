#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace seed {
// Index of a registered material. Tiles store it in one byte.
using MaterialId = std::uint8_t;

// How a generated atlas tile is drawn: flat noise, water bands, timber planks, or a round sprite
// with a domed normal.
enum class Pattern : std::uint8_t { speckle, water, planks, round };

struct MaterialDesc {
    const char* name{};         // Unique; must outlive the registry (a string literal is typical).
    std::array<int, 3> color{}; // Base RGB, 0-255.
    Pattern pattern{Pattern::speckle};
    int variation{23}; // Per-pixel noise range; must be positive.
    // Optional packed BC3 texture drawn instead of the generated tile, without a normal map.
    const char* texture{};
    // How many tiles one copy of the texture covers when drawn with Renderer::ground, so
    // it repeats seamlessly across neighbouring tiles. 1 draws the whole texture on every tile.
    float texture_scale{1};
    // A texture that is a horizontal strip of `frames` equal pictures is an animation, shown at
    // `fps` frames per second (0 holds the first unless something picks a frame).
    unsigned frames{1};
    float fps{};
    // Walking speed on this ground, as a multiple of a character's own speed (0.1 to 4).
    float speed{1};
    // Labels for scripts, separated by spaces, such as "slippery hurts"; the engine gives them no
    // meaning. Null for none.
    const char* tags{};
};

// The game's materials, registered once at startup before the renderer and world generation
// start. IDs are assigned in registration order, so a game that always registers in the same order
// always gets the same IDs and the same generated atlas.
class Materials final {
public:
    static constexpr std::size_t capacity = 64;
    MaterialId add(const MaterialDesc& desc) {
        if (!desc.name || !*desc.name) throw std::invalid_argument("A material needs a name");
        if (desc.variation <= 0) throw std::invalid_argument("Material variation must be positive");
        if (!(desc.texture_scale >= 1 && desc.texture_scale <= 256))
            throw std::invalid_argument("Material texture scale must be from 1 to 256 tiles");
        if (desc.frames < 1 || desc.frames > 64) throw std::invalid_argument("A material has 1 to 64 frames");
        if (!(desc.fps >= 0 && desc.fps <= 60))
            throw std::invalid_argument("Animation speed must be 0 to 60 fps");
        if (!(desc.speed >= 0.1F && desc.speed <= 4))
            throw std::invalid_argument("Material speed must be from 0.1 to 4");
        if (desc.tags) check_tags(desc.tags);
        for (int c : desc.color)
            if (c < 0 || c > 255) throw std::invalid_argument("Material colour out of range");
        for (std::size_t i = 0; i < size_; ++i)
            if (std::string_view(entries_[i].name) == desc.name)
                throw std::invalid_argument(std::string("Duplicate material: ") + desc.name);
        if (size_ == capacity) throw std::length_error("Material registry is full");
        entries_[size_] = desc;
        return static_cast<MaterialId>(size_++);
    }
    MaterialId find(std::string_view name) const {
        for (std::size_t i = 0; i < size_; ++i)
            if (entries_[i].name == name) return static_cast<MaterialId>(i);
        throw std::out_of_range("Unknown material: " + std::string(name));
    }
    const MaterialDesc& operator[](MaterialId id) const {
        if (id >= size_) throw std::out_of_range("Unregistered material ID");
        return entries_[id];
    }
    std::size_t size() const { return size_; }

    // Whether the material's tags include `tag`.
    bool tagged(MaterialId id, std::string_view tag) const {
        std::string_view tags = (*this)[id].tags ? (*this)[id].tags : "";
        while (!tags.empty()) {
            const auto end = tags.find(' ');
            if (tags.substr(0, end) == tag) return true;
            if (end == std::string_view::npos) break;
            tags.remove_prefix(end + 1);
        }
        return false;
    }

private:
    // At most 8 tags of letters, digits, '_' and '-', one space apart.
    static void check_tags(std::string_view tags) {
        std::size_t count = 0, length = 0;
        for (std::size_t i = 0; i <= tags.size(); ++i) {
            const char c = i < tags.size() ? tags[i] : ' ';
            if (c == ' ') {
                if (!length)
                    throw std::invalid_argument("Material tags are words separated by single spaces");
                if (++count > 8) throw std::invalid_argument("A material has at most 8 tags");
                length = 0;
            } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                       c == '_' || c == '-') {
                if (++length > 32) throw std::invalid_argument("Material tags are at most 32 characters");
            } else
                throw std::invalid_argument("Material tags use letters, digits, '_' and '-'");
        }
    }
    std::array<MaterialDesc, capacity> entries_{};
    std::size_t size_{};
};
} // namespace seed
