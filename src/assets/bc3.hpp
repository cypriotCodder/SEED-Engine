#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace seed {
// Small offline BC3 encoder. Endpoint fitting favors low cooker complexity;
// the runtime uploads the resulting blocks directly, with no CPU expansion.
inline std::vector<std::uint8_t> encode_bc3(std::span<const std::uint8_t> rgba, unsigned width,
                                            unsigned height) {
    if (!width || !height || width % 4 || height % 4 || width > 4096 || height > 4096 ||
        rgba.size() != std::size_t(width) * height * 4)
        throw std::invalid_argument("BC3 expects RGBA and dimensions divisible by four, up to 4096");
    std::vector<std::uint8_t> result;
    result.reserve(std::size_t(width) * height);
    auto byte = [&](unsigned n) {
        result.push_back(static_cast<std::uint8_t>(n));
    };
    for (unsigned by = 0; by < height; by += 4)
        for (unsigned bx = 0; bx < width; bx += 4) {
            std::array<std::array<unsigned, 4>, 16> pixels{};
            std::array<unsigned, 4> lo{255, 255, 255, 255}, hi{};
            for (unsigned i = 0; i < 16; ++i)
                for (unsigned c = 0; c < 4; ++c) {
                    const auto v = rgba[((by + i / 4) * width + bx + i % 4) * 4 + c];
                    pixels[i][c] = v;
                    lo[c] = std::min(lo[c], unsigned(v));
                    hi[c] = std::max(hi[c], unsigned(v));
                }
            byte(hi[3]);
            byte(lo[3]);
            std::array<unsigned, 8> alpha{hi[3], lo[3]};
            for (unsigned i = 1; i <= 6; ++i)
                alpha[i + 1] = ((7 - i) * hi[3] + i * lo[3]) / 7;
            std::uint64_t alpha_bits = 0;
            for (unsigned i = 0; i < 16; ++i) {
                unsigned best = 0, distance = 256;
                for (unsigned j = 0; j < 8; ++j) {
                    const auto d = static_cast<unsigned>(std::abs(int(pixels[i][3]) - int(alpha[j])));
                    if (d < distance) {
                        distance = d;
                        best = j;
                    }
                }
                alpha_bits |= std::uint64_t(best) << (3 * i);
            }
            for (unsigned i = 0; i < 6; ++i)
                byte(static_cast<unsigned>(alpha_bits >> (8 * i)));
            auto pack = [](auto color) {
                return ((color[0] * 31 / 255) << 11) | ((color[1] * 63 / 255) << 5) | (color[2] * 31 / 255);
            };
            unsigned c0 = pack(hi), c1 = pack(lo);
            if (c0 == c1) {
                if (c0 < 65535)
                    ++c0;
                else
                    --c1;
            }
            byte(c0);
            byte(c0 >> 8);
            byte(c1);
            byte(c1 >> 8);
            auto unpack = [](unsigned c) -> std::array<unsigned, 3> {
                return {((c >> 11) & 31) * 255 / 31, ((c >> 5) & 63) * 255 / 63, (c & 31) * 255 / 31};
            };
            std::array<std::array<unsigned, 3>, 4> colors{unpack(c0), unpack(c1)};
            for (unsigned c = 0; c < 3; ++c) {
                colors[2][c] = (2 * colors[0][c] + colors[1][c]) / 3;
                colors[3][c] = (colors[0][c] + 2 * colors[1][c]) / 3;
            }
            std::uint32_t indices = 0;
            for (unsigned i = 0; i < 16; ++i) {
                unsigned best = 0, distance = std::numeric_limits<unsigned>::max();
                for (unsigned j = 0; j < 4; ++j) {
                    unsigned d = 0;
                    for (unsigned c = 0; c < 3; ++c) {
                        const int delta = int(pixels[i][c]) - int(colors[j][c]);
                        d += static_cast<unsigned>(delta * delta);
                    }
                    if (d < distance) {
                        distance = d;
                        best = j;
                    }
                }
                indices |= best << (2 * i);
            }
            for (unsigned i = 0; i < 4; ++i)
                byte(indices >> (8 * i));
        }
    return result;
}
} // namespace seed
