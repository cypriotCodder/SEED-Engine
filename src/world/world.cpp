#include "world/world.hpp"
#include "io/binary.hpp"
#include "io/storage.hpp"
#include "world/chunk_file.hpp"
#include <chrono>

namespace seed {
World::World(Jobs& jobs, std::uint64_t seed, std::filesystem::path directory, ReadPath read_path)
    : jobs_(jobs), seed_(seed), directory_(std::move(directory)), read_path_(std::move(read_path)) {
    if (!read_path_)
        read_path_ = [](const std::filesystem::path& path) {
            return path;
        };
    std::filesystem::create_directories(directory_);
    const auto metadata = directory_ / "world.seed";
    if (std::filesystem::exists(read_path_(metadata))) {
        const auto bytes = read_blob(read_path_(metadata));
        Reader input(bytes);
        if (input.u32() != 0x444c5257 || input.u32() != 1 || input.u32() != generator_version ||
            input.u64() != seed_ || !input.done())
            throw std::runtime_error("Save seed or generator version mismatch");
    } else {
        Bytes output;
        output.u32(0x444c5257);
        output.u32(1);
        output.u32(generator_version);
        output.u64(seed_);
        write_blob(metadata, output.data);
    }
    const auto legacy = directory_ / "0_0.bodies";
    if (std::filesystem::exists(legacy)) {
        // Only the private working copy is migrated. Original flat files remain untouched.
        auto chunk = std::make_unique<Chunk>();
        generate(*chunk, seed_, {});
        const auto origin = path({});
        if (std::filesystem::exists(origin)) {
            const auto bytes = read_blob(origin);
            Reader input(bytes);
            input.u32();
            if (input.u32() != 1) throw std::runtime_error("Ambiguous legacy and chunk-owned building saves");
            decode_chunk(bytes, seed_, {}, *chunk);
        }
        decode_legacy_bodies(read_blob(legacy), seed_, *chunk);
        ChunkBodies baseline;
        generate_structures(baseline, seed_, {});
        write_blob(origin, encode_chunk(seed_, {}, *chunk, baseline));
        std::filesystem::remove(legacy);
    }
    for (auto& slot : slots_) {
        slot.world = this;
        slot.chunk = &pool_.get(pool_.create());
    }
}
World::~World() {
    jobs_.wait(group_);
}
std::filesystem::path World::path(ChunkCoord coord) const {
    return directory_ / (std::to_string(coord.x) + "_" + std::to_string(coord.y) + ".chunk");
}
void World::check_errors() const {
    for (const auto& slot : slots_)
        if (slot.state.load(std::memory_order_acquire) == State::failed) std::rethrow_exception(slot.error);
}
void World::generate_job(void* context) noexcept {
    auto& slot = *static_cast<Slot*>(context);
    try {
        const auto start = std::chrono::steady_clock::now();
        generate(*slot.chunk, slot.world->seed_, slot.coord);
        const auto ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
                .count());
        slot.world->generated_.fetch_add(1, std::memory_order_relaxed);
        slot.world->generation_ns_.fetch_add(ns, std::memory_order_relaxed);
        auto maximum = slot.world->generation_max_ns_.load(std::memory_order_relaxed);
        while (maximum < ns && !slot.world->generation_max_ns_.compare_exchange_weak(
                                   maximum, ns, std::memory_order_relaxed)) {}
        slot.world->load(slot);
        slot.state.store(State::ready, std::memory_order_release);
    } catch (...) {
        slot.error = std::current_exception();
        slot.state.store(State::failed, std::memory_order_release);
    }
}
void World::save_job(void* context) noexcept {
    auto& slot = *static_cast<Slot*>(context);
    try {
        slot.world->write(slot);
        slot.state.store(State::saved, std::memory_order_release);
    } catch (...) {
        slot.error = std::current_exception();
        slot.state.store(State::failed, std::memory_order_release);
    }
}
void World::load(Slot& slot) {
    const auto file = read_path_(path(slot.coord));
    if (!std::filesystem::exists(file)) return;
    decode_chunk(read_blob(file), seed_, slot.coord, *slot.chunk);
}
void World::write(Slot& slot) {
    if (!slot.chunk->dirty) return;
    // Regenerating the recipe is cheap and keeps a second copy of it out of every chunk slot.
    ChunkBodies baseline;
    generate_structures(baseline, seed_, slot.coord);
    const auto file = path(slot.coord);
    if (chunk_matches_baseline(*slot.chunk, baseline) && !std::filesystem::exists(read_path_(file))) {
        slot.chunk->dirty = false;
        return;
    }
    write_blob(file, encode_chunk(seed_, slot.coord, *slot.chunk, baseline));
    slot.chunk->dirty = false;
}
void World::stream(ChunkCoord center) {
    check_errors();
    for (auto& slot : slots_) {
        auto state = slot.state.load(std::memory_order_acquire);
        if (state == State::saved) {
            slot.state = State::empty;
            state = State::empty;
        }
        if (state == State::ready) {
            const bool keep = nearby(slot.coord, center, 3);
            if (keep && hooks_.activate) hooks_.activate(hooks_.context, slot.coord, *slot.chunk);
            slot.state = keep ? State::active : State::empty;
            state = slot.state.load();
        }
        if (state == State::active && !nearby(slot.coord, center, 3)) {
            if (hooks_.release) hooks_.release(hooks_.context, slot.coord, *slot.chunk);
            if (slot.chunk->dirty) {
                slot.state = State::saving;
                jobs_.submit({save_job, &slot, &group_});
            } else
                slot.state = State::empty;
        }
    }
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx) {
            const ChunkCoord coord{checked_add(center.x, dx), checked_add(center.y, dy)};
            Slot* empty = nullptr;
            bool exists = false;
            for (auto& slot : slots_) {
                const auto state = slot.state.load(std::memory_order_acquire);
                if (state == State::empty) {
                    if (!empty) empty = &slot;
                } else if (slot.coord == coord) {
                    exists = true;
                    break;
                }
            }
            if (!exists && empty) {
                empty->coord = coord;
                empty->error = nullptr;
                empty->state = State::generating;
                jobs_.submit({generate_job, empty, &group_});
            }
        }
}
void World::settle(ChunkCoord center) {
    stream(center);
    jobs_.wait(group_);
    stream(center);
    jobs_.wait(group_);
    stream(center);
}
void World::save() {
    jobs_.wait(group_);
    check_errors();
    for (auto& slot : slots_) {
        const auto state = slot.state.load();
        if (state == State::active && hooks_.store) hooks_.store(hooks_.context, slot.coord, *slot.chunk);
        if (state == State::active || state == State::ready) write(slot);
    }
}
const Tile* World::tile(WorldPosition position) const {
    position.move({});
    for (const auto& slot : slots_)
        if (slot.state.load(std::memory_order_acquire) == State::active && slot.coord == position.chunk)
            return &slot.chunk->tiles[static_cast<std::size_t>(position.local.y) * chunk_side +
                                      static_cast<std::size_t>(position.local.x)];
    return nullptr;
}
bool World::remove_tree(WorldPosition position) {
    position.move({});
    for (auto& slot : slots_)
        if (slot.state.load(std::memory_order_acquire) == State::active && slot.coord == position.chunk) {
            const auto index = static_cast<std::size_t>(position.local.y) * chunk_side +
                               static_cast<std::size_t>(position.local.x);
            if (!slot.chunk->tiles[index].tree) return false;
            slot.chunk->tiles[index].tree = false;
            slot.chunk->changes[index] |= 1;
            slot.chunk->dirty = true;
            return true;
        }
    return false;
}
bool World::dig(WorldPosition position) {
    position.move({});
    for (auto& slot : slots_)
        if (slot.state.load(std::memory_order_acquire) == State::active && slot.coord == position.chunk) {
            const auto index = static_cast<std::size_t>(position.local.y) * chunk_side +
                               static_cast<std::size_t>(position.local.x);
            auto& tile = slot.chunk->tiles[index];
            if (tile.elevation < 0) return false;
            tile.tree = false;
            tile.elevation = -0.1F;
            tile.material = 0;
            slot.chunk->changes[index] |= 3;
            slot.chunk->dirty = true;
            return true;
        }
    return false;
}
} // namespace seed
