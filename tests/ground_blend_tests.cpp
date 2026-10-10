#include "render/renderer.hpp"
#include <iostream>
#include <stdexcept>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

using seed::Renderer;
// Materials 0 water (blend 0), 1 sand (10), 2 grass (20), 3 road (20); 4 is not registered.
std::array<std::uint8_t, seed::Materials::capacity> blends() {
    std::array<std::uint8_t, seed::Materials::capacity> blend{};
    blend[1] = 10;
    blend[2] = 20;
    blend[3] = 20;
    return blend;
}

std::size_t overlays(seed::MaterialId material, const std::array<seed::MaterialId, 8>& neighbours,
                     std::array<seed::GroundOverlay, 8>& out) {
    out = {};
    return seed::ground_overlays(blends(), 4, material, neighbours, out);
}

void selection() {
    std::array<seed::GroundOverlay, 8> out{};
    check(overlays(1, {1, 1, 1, 1, 1, 1, 1, 1}, out) == 0, "Uniform ground has no overlays");
    check(overlays(2, {0, 1, 0, 1, 0, 1, 0, 1}, out) == 0, "Lower neighbours do not blend over");
    check(overlays(0, {0, 0, 0, 0, 0, 0, 0, 0}, out) == 0, "Blend 0 never spreads");
    check(overlays(4, {2, 2, 2, 2, 2, 2, 2, 2}, out) == 0, "Unregistered tiles draw no overlays");
    check(overlays(1, {4, 4, 4, 4, 4, 4, 4, 4}, out) == 0, "Unregistered neighbours are ignored");

    // Sand bordered by grass to the east (and both its corners), water to the west.
    check(overlays(1, {2, 0, 1, 1, 2, 0, 2, 0}, out) == 1, "One blending neighbour");
    check(out[0].material == 2 && out[0].edges == Renderer::edge_east,
          "Corners beside a blending side are covered by that side");

    // Water with sand to the north, grass in the south-east corner only.
    check(overlays(0, {0, 0, 1, 0, 1, 1, 2, 0}, out) == 2, "Two blending neighbours");
    check(out[0].material == 1 && out[1].material == 2, "Lower blends are drawn first");
    check(out[0].edges == Renderer::edge_north, "North side");
    check(out[1].edges == Renderer::edge_south_east, "A lone corner fades in");

    // Equal blends are ordered by material ID, whatever order they appear in.
    check(overlays(0, {3, 2, 0, 0, 0, 0, 0, 0}, out) == 2 && out[0].material == 2 && out[1].material == 3,
          "Equal blends ordered by ID");
    check(out[0].edges == Renderer::edge_west && out[1].edges == Renderer::edge_east,
          "Sides kept per material");
}
} // namespace

int main() {
    try {
        selection();
        std::cout << "Ground blend checks passed.\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
