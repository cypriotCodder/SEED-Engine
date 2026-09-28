#include "assets/pack_writer.hpp"
#include "io/binary.hpp"
#include "io/storage.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <set>

namespace {
struct Image {
    unsigned width{}, height{};
    std::vector<std::uint8_t> pixels;
};
Image read_tga(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::array<std::uint8_t, 18> header{};
    file.read(reinterpret_cast<char*>(header.data()), header.size());
    if (!file || header[1] || header[2] != 2 || (header[16] != 24 && header[16] != 32) || (header[17] & 16))
        throw std::runtime_error("Cooker expects an uncompressed, left-origin 24/32-bit TGA");
    Image image;
    image.width = header[12] | (unsigned(header[13]) << 8);
    image.height = header[14] | (unsigned(header[15]) << 8);
    if (!image.width || !image.height || image.width % 4 || image.height % 4 || image.width > 4096 ||
        image.height > 4096)
        throw std::runtime_error("TGA dimensions must be multiples of four, up to 4096");
    image.pixels.resize(std::size_t(image.width) * image.height * 4);
    file.seekg(header[0], std::ios::cur);
    for (unsigned y = 0; y < image.height; ++y)
        for (unsigned x = 0; x < image.width; ++x) {
            std::array<std::uint8_t, 4> color{0, 0, 0, 255};
            file.read(reinterpret_cast<char*>(color.data()), header[16] / 8);
            const auto row = (header[17] & 32) ? image.height - 1 - y : y;
            const auto i = (std::size_t(row) * image.width + x) * 4;
            image.pixels[i] = color[2];
            image.pixels[i + 1] = color[1];
            image.pixels[i + 2] = color[0];
            image.pixels[i + 3] = color[3];
        }
    if (!file) throw std::runtime_error("Truncated TGA pixels");
    return image;
}
Image flame() {
    Image image{32, 32, std::vector<std::uint8_t>(32 * 32 * 4)};
    for (unsigned y = 0; y < 32; ++y)
        for (unsigned x = 0; x < 32; ++x) {
            const float px = (static_cast<float>(x) - 15.5F) / 16, py = static_cast<float>(y) / 32;
            const float width = 0.65F * (1 - py) + 0.1F * std::sin(py * 17);
            const auto i = (y * 32 + x) * 4;
            image.pixels[i] = 255;
            image.pixels[i + 1] = static_cast<std::uint8_t>(90 + 160 * (1 - py));
            image.pixels[i + 2] = static_cast<std::uint8_t>(15 + 90 * (1 - py));
            image.pixels[i + 3] = std::abs(px) < width ? 255 : 0;
        }
    return image;
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc < 3) throw std::runtime_error("Usage: seed_cook OUTPUT.pak (--builtin | INPUT.tga ...)");
        const bool builtin = std::string_view(argv[2]) == "--builtin";
        if ((builtin && argc != 3) || argc - 2 > 64) throw std::runtime_error("Invalid cooker arguments");
        std::vector<seed::PackImage> images;
        for (int i = 2; i < argc; ++i) {
            auto image = builtin ? flame() : read_tga(argv[i]);
            const auto name = builtin ? std::string("flame") : std::filesystem::path(argv[i]).stem().string();
            images.push_back({name, image.width, image.height, std::move(image.pixels)});
        }
        seed::write_blob(argv[1], seed::pack_payload(images));
        std::cout << "Cooked " << argv[1] << " (" << std::filesystem::file_size(argv[1]) << " bytes).\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
