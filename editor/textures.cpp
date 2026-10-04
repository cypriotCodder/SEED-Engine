#include "textures.hpp"
#include "io/storage.hpp"
#include "project/assets.hpp"
#include <algorithm>
#include <cctype>
#include <memory>

#include "stb_image.h" // Compiled in stb_image.c, outside the strict warning flags.

namespace seed::editor {
namespace fs = std::filesystem;
namespace {
constexpr const char* extensions[] = {".png", ".jpg", ".jpeg", ".tga"};

bool is_image(const fs::path& path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return std::find(std::begin(extensions), std::end(extensions), extension) != std::end(extensions);
}

// The image file for each texture name, in name order.
std::vector<std::pair<std::string, fs::path>> texture_files(const fs::path& project) {
    std::vector<std::pair<std::string, fs::path>> files;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(texture_folder(project), error))
        if (entry.is_regular_file() && is_image(entry.path()) &&
            valid_terrain_name(entry.path().stem().string()))
            files.emplace_back(entry.path().stem().string(), entry.path());
    std::sort(files.begin(), files.end());
    return files;
}

unsigned multiple_of_four(int side) {
    const auto rounded = static_cast<unsigned>(std::max(4, (side + 2) / 4 * 4));
    return std::min(rounded, 4096U);
}
} // namespace

fs::path texture_folder(const fs::path& project) {
    return project / "assets" / "textures";
}

fs::path texture_pack(const fs::path& project) {
    return project / ".seed" / "textures.pak";
}

std::vector<std::string> list_textures(const fs::path& project) {
    std::vector<std::string> names;
    for (const auto& [name, file] : texture_files(project))
        names.push_back(name);
    return names;
}

PackImage read_texture(const fs::path& file, const std::string& name) {
    const auto bytes = read_text(file, 64 * 1024 * 1024);
    int width{}, height{}, channels{};
    std::unique_ptr<stbi_uc, void (*)(void*)> pixels(
        stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()),
                              &width, &height, &channels, 4),
        stbi_image_free);
    if (!pixels) throw std::runtime_error(file.filename().string() + ": " + stbi_failure_reason());
    // Nearest-neighbour resampling to sides that are multiples of four, and flipped so the bottom
    // row comes first, as textures are sampled.
    PackImage image{name, multiple_of_four(width), multiple_of_four(height), {}};
    image.rgba.resize(std::size_t(image.width) * image.height * 4);
    for (unsigned y = 0; y < image.height; ++y)
        for (unsigned x = 0; x < image.width; ++x) {
            const auto sx = std::min<unsigned>(x * static_cast<unsigned>(width) / image.width,
                                               static_cast<unsigned>(width) - 1);
            const auto sy =
                std::min<unsigned>((image.height - 1 - y) * static_cast<unsigned>(height) / image.height,
                                   static_cast<unsigned>(height) - 1);
            std::copy_n(pixels.get() + (std::size_t(sy) * static_cast<unsigned>(width) + sx) * 4, 4,
                        image.rgba.begin() +
                            static_cast<std::ptrdiff_t>((std::size_t(y) * image.width + x) * 4));
        }
    return image;
}

std::vector<std::uint8_t> decode_image(const unsigned char* bytes, std::size_t size, int& width,
                                       int& height) {
    int channels{};
    std::unique_ptr<stbi_uc, void (*)(void*)> pixels(
        stbi_load_from_memory(bytes, static_cast<int>(size), &width, &height, &channels, 4), stbi_image_free);
    if (!pixels) throw std::runtime_error(std::string("Image: ") + stbi_failure_reason());
    return {pixels.get(), pixels.get() + std::size_t(width) * std::size_t(height) * 4};
}

std::string import_texture(const fs::path& project, const fs::path& image) {
    if (!is_image(image)) throw std::runtime_error("Textures are PNG, JPG or TGA images");
    read_texture(image, "check"); // Refuse anything that will not decode, before copying it.
    // A name from the file name, keeping only the characters names allow.
    std::string base;
    for (const char c : image.stem().string())
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')
            base += c;
        else if (c == ' ' || c == '.')
            base += '_';
    if (base.empty()) base = "texture";
    if (base.size() > 60) base.resize(60);
    const auto names = list_textures(project);
    auto name = base;
    for (int n = 2; std::find(names.begin(), names.end(), name) != names.end(); ++n)
        name = base + "_" + std::to_string(n);
    auto extension = image.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    fs::create_directories(texture_folder(project));
    fs::copy_file(image, texture_folder(project) / (name + extension));
    return name;
}

bool cook_textures(const fs::path& project) {
    const auto files = texture_files(project);
    const auto pack = texture_pack(project), stamp = project / ".seed" / "textures.stamp";
    // The stamp lists each file with its size and time; the pack is rebuilt when that changes.
    std::string listing;
    for (const auto& [name, file] : files)
        listing +=
            name + " " + std::to_string(fs::file_size(file)) + " " +
            std::to_string(static_cast<long long>(fs::last_write_time(file).time_since_epoch().count())) +
            "\n";
    if (files.empty()) {
        const bool had = fs::exists(pack);
        fs::remove(pack);
        fs::remove(stamp);
        return had;
    }
    if (fs::exists(pack) && fs::exists(stamp) && read_text(stamp) == listing) return false;
    std::vector<PackImage> images;
    for (const auto& [name, file] : files)
        images.push_back(read_texture(file, name));
    fs::create_directories(pack.parent_path());
    write_blob(pack, pack_payload(images));
    write_text(stamp, listing);
    return true;
}
} // namespace seed::editor
