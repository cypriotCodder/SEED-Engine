#include "assets/bc3.hpp"
#include "core/ecs.hpp"
#include "io/binary.hpp"
#include "physics/collision.hpp"
#include "world/generator.hpp"
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Exception, class F>
void throws(F&& f) {
    try {
        f();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error("Expected exception not thrown");
}
} // namespace
int main() {
    try {
        seed::Arena arena(1024 * 1024);
        arena.allocate(3, 1);
        check(reinterpret_cast<std::uintptr_t>(arena.allocate(32, 64)) % 64 == 0, "Arena alignment");
        const auto used = arena.used();
        throws<std::bad_alloc>([&] { arena.allocate(2 * 1024 * 1024, 8); });
        check(arena.used() == used, "Failed allocation mutated arena");
        throws<std::invalid_argument>([&] { arena.allocate(8, 3); });
        arena.reset();
        check(arena.used() == 0, "Arena reset");
        seed::Pool<int, 2> pool;
        const auto a = pool.create(7), b = pool.create(9);
        check(pool.get(a) == 7 && pool.get(b) == 9, "Pool values");
        throws<std::bad_alloc>([&] { pool.create(11); });
        pool.destroy(a);
        check(pool.create(12) == a && pool.get(a) == 12, "Pool reuse");
        seed::Entities entities(arena);
        seed::SparseSet<int> components(arena);
        auto first = entities.create(), second = entities.create();
        components.add(first, 42);
        components.add(second, 99);
        components.remove(first);
        entities.destroy(first);
        const auto replacement = entities.create();
        check(first.index == replacement.index && first != replacement, "Entity generation");
        check(!entities.alive(first) && entities.alive(replacement), "Stale entity");
        check(!components.find(first) && *components.find(second) == 99, "Sparse-set swap remove");
        throws<std::out_of_range>([&] { entities.destroy(first); });
        seed::WorldPosition position{};
        position.move({-0.25F, -32.25F});
        check(position.chunk == seed::ChunkCoord{-1, -2} && position.local.x == 31.75F &&
                  position.local.y == 31.75F,
              "Negative world normalization");
        position.move({0.25F, 32.25F});
        check(position.chunk == seed::ChunkCoord{} && position.local.x == 0 && position.local.y == 0,
              "Coordinate round trip");
        for (unsigned wave = 8; wave <= 128; wave *= 2) {
            check(seed::perlin(42, {-1, 0}, {32, 7.25F}, wave) == seed::perlin(42, {0, 0}, {0, 7.25F}, wave),
                  "Noise chunk seam");
            const auto n = seed::perlin(
                42, {std::numeric_limits<std::int64_t>::max(), std::numeric_limits<std::int64_t>::min()},
                {0.5F, 31.5F}, wave);
            check(std::isfinite(n) && std::abs(n) <= 1.5F, "Distant noise range");
        }
        seed::Chunk chunk_a, chunk_b, chunk_c;
        seed::generate(chunk_a, 123, {0, 0});
        seed::generate(chunk_b, 123, {0, 0});
        seed::generate(chunk_c, 124, {0, 0});
        bool different = false;
        for (std::size_t i = 0; i < chunk_a.tiles.size(); ++i) {
            check(chunk_a.tiles[i].elevation == chunk_b.tiles[i].elevation &&
                      chunk_a.tiles[i].tree == chunk_b.tiles[i].tree,
                  "Generator repeatability");
            different |= chunk_a.tiles[i].elevation != chunk_c.tiles[i].elevation;
        }
        check(different, "Seed must affect generated world");
        seed::Contact contact, reverse;
        const seed::Box box{{0, 0}, {1, 1}, 0}, overlap{{1.5F, 0}, {1, 1}, 0};
        check(seed::collide(box, overlap, contact) && std::abs(contact.depth - 0.5F) < 1e-5F &&
                  contact.normal.x > 0,
              "AABB penetration");
        check(seed::collide(overlap, box, reverse) && seed::dot(contact.normal, reverse.normal) < -0.99F,
              "SAT symmetry");
        check(!seed::collide(box, {{2, 0}, {1, 1}, 0}, contact), "Touching boxes are not penetrating");
        check(seed::collide(box, {{0, 0}, {0.1F, 0.1F}, 0.7F}, contact) && contact.depth > 1,
              "Contained rotated box");
        check(!seed::collide({{0, 0}, {2, 0.1F}, 0.785398F}, {{0, 1}, {2, 0.1F}, 0.785398F}, contact),
              "SAT rejects overlapping bounds of separate slivers");
        constexpr std::array<std::uint8_t, 9> crc_sample{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
        check(seed::crc32(crc_sample) == 0xcbf43926U, "CRC standard check vector");
        seed::Bytes bytes;
        bytes.u16(0xabcd);
        bytes.u64(0x0123456789abcdefULL);
        seed::Reader reader(bytes.data);
        check(reader.u16() == 0xabcd && reader.u64() == 0x0123456789abcdefULL && reader.done(),
              "Little-endian record round trip");
        throws<std::runtime_error>([&] { reader.u8(); });
        std::array<std::uint8_t, 64> opaque_red{};
        for (std::size_t i = 0; i < 16; ++i) {
            opaque_red[4 * i] = 255;
            opaque_red[4 * i + 3] = 255;
        }
        const auto bc = seed::encode_bc3(opaque_red, 4, 4);
        check(bc.size() == 16 && bc[0] == 255 && bc[1] == 255, "BC3 block size and opaque alpha");
        throws<std::invalid_argument>([&] { seed::encode_bc3(opaque_red, 3, 4); });
        std::cout << "Allocator, entity, coordinate, noise, collision and binary-codec checks passed.\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
