#include "assets/pack.hpp"
#include "io/storage.hpp"
#include <fstream>
#include <iostream>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F>
void rejects(F&& action) {
    try {
        action();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error("Corrupt storage was accepted");
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::runtime_error("Expected test directory and asset archive");
        const auto directory = std::filesystem::path(argv[1]);
        std::filesystem::create_directories(directory);
        const auto file = directory / "roundtrip.test";
        std::vector<std::uint8_t> data(70000);
        for (std::size_t i = 0; i < data.size(); ++i)
            data[i] = static_cast<std::uint8_t>(i * 17 + i / 13);
        seed::write_blob(file, data);
        check(seed::read_blob(file) == data, "LZ4 storage round trip");
        {
            std::fstream corrupt(file, std::ios::binary | std::ios::in | std::ios::out);
            corrupt.seekg(-1, std::ios::end);
            char byte{};
            corrupt.read(&byte, 1);
            byte ^= 0x5a;
            corrupt.seekp(-1, std::ios::end);
            corrupt.write(&byte, 1);
            check(bool(corrupt), "Corruption fixture write");
        }
        rejects([&] { seed::read_blob(file); });
        seed::write_blob(file, data);
        {
            std::fstream corrupt(file, std::ios::binary | std::ios::in | std::ios::out);
            corrupt.seekp(4);
            const char unsupported = 127;
            corrupt.write(&unsupported, 1);
            check(bool(corrupt), "Version fixture write");
        }
        rejects([&] { seed::read_blob(file); });
        seed::write_blob(file, data);
        check(seed::read_blob(file) == data, "Atomic replacement of existing file");
        seed::Pack pack(argv[2]);
        const auto& flame = pack.texture("flame");
        check(flame.width == 32 && flame.height == 32 && flame.blocks.size() == 1024,
              "Cooked BC3 asset layout");
        rejects([&] { pack.texture("missing"); });
        std::cout << "Compressed storage, corruption rejection and archive checks passed.\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
