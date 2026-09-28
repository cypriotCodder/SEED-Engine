#pragma once
#include "assets/bc3.hpp"
#include "io/binary.hpp"
#include <set>
#include <string>
#include <vector>

namespace seed {
// An image to pack: RGBA, bottom row first (the way textures are sampled), sides multiples of four.
struct PackImage {
    std::string name;
    unsigned width{}, height{};
    std::vector<std::uint8_t> rgba;
};

// The payload of an asset pack (see Pack): each image compressed to BC3. Write it with write_blob.
inline std::vector<std::uint8_t> pack_payload(const std::vector<PackImage>& images) {
    if (images.size() > 64) throw std::length_error("An asset pack holds at most 64 textures");
    Bytes out;
    out.u32(0x4b504453);
    out.u32(1);
    out.u32(static_cast<std::uint32_t>(images.size()));
    std::set<std::string> names;
    for (const auto& image : images) {
        if (image.name.empty() || image.name.size() > 128 || !names.insert(image.name).second)
            throw std::invalid_argument("Invalid or duplicate texture name: " + image.name);
        const auto blocks = encode_bc3(image.rgba, image.width, image.height);
        out.u16(static_cast<std::uint16_t>(image.name.size()));
        out.data.insert(out.data.end(), image.name.begin(), image.name.end());
        out.u8(1); // BC3
        out.u16(static_cast<std::uint16_t>(image.width));
        out.u16(static_cast<std::uint16_t>(image.height));
        out.u32(static_cast<std::uint32_t>(blocks.size()));
        out.u32(crc32(blocks));
        out.data.insert(out.data.end(), blocks.begin(), blocks.end());
    }
    return out.data;
}
} // namespace seed
