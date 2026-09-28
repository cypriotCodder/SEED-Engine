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

private:
    std::array<MaterialDesc, capacity> entries_{};
    std::size_t size_{};
};
} // namespace seed
