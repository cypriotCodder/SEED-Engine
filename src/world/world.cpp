#include "world/world.hpp"
#include "io/binary.hpp"
#include "io/storage.hpp"
#include "world/chunk_file.hpp"
#include <chrono>

namespace seed {
namespace {
constexpr std::uint32_t world_file_magic = 0x444c5257, world_file_version = 2;
}
World::World(Jobs& jobs, std::uint64_t game_id, const WorldGenerator& generator, std::uint64_t seed,
             std::filesystem::path directory, ReadPath read_path)
    : jobs_(jobs),
      generator_(generator),
      game_id_(game_id),
      seed_(seed),
      directory_(std::move(directory)),
      read_path_(std::move(read_path)) {
    validate(generator_);
    if (!read_path_)
        read_path_ = [](const std::filesystem::path& path) {
            return path;
        };
    if (std::filesystem::exists(directory_ / "0_0.bodies"))
        throw std::runtime_error(
            "Legacy standalone building saves are unsupported; choose a new save directory");
    std::filesystem::create_directories(directory_);
    const auto metadata = directory_ / "world.seed";
    if (std::filesystem::exists(read_path_(metadata))) {
        const auto bytes = read_blob(read_path_(metadata));
        Reader input(bytes);
        if (input.u32() != world_file_magic) throw std::runtime_error("Invalid world.seed file");
        if (input.u32() != world_file_version)
            throw std::runtime_error(
                "Unsupported save format from an older build; choose a new save directory");
        const auto game = input.u64(), generator_hash = input.u64();
        const auto version = input.u32();
        const auto seed = input.u64();
        if (!input.done()) throw std::runtime_error("Invalid world.seed file");
        if (game != game_id_) throw std::runtime_error("This save belongs to another game");
        if (generator_hash != generator_id(generator_))
            throw std::runtime_error("This save was made by another world generator");
        if (version != generator_.version)
            throw std::runtime_error("This save was made by another version of the world generator");
        if (seed != seed_) throw std::runtime_error("This save uses a different seed");
    } else {
        Bytes output;
        output.u32(world_file_magic);
        output.u32(world_file_version);
        output.u64(game_id_);
        output.u64(generator_id(generator_));
        output.u32(generator_.version);
        output.u64(seed_);
        write_blob(metadata, output.data);
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
        fill_chunk(slot.world->generator_, slot.world->seed_, slot.coord, *slot.chunk);
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
    decode_chunk(read_blob(file), generator_, seed_, slot.coord, *slot.chunk);
}
void World::write(Slot& slot) {
    if (!slot.chunk->dirty) return;
    // Regenerating the recipe is cheap and keeps a second copy of it out of every chunk slot.
    ChunkBodies baseline;
    generate_structures(generator_, seed_, slot.coord, baseline);
    const auto file = path(slot.coord);
    if (chunk_matches_baseline(*slot.chunk, baseline) && !std::filesystem::exists(read_path_(file))) {
        slot.chunk->dirty = false;
        return;
    }
    write_blob(file, encode_chunk(generator_, seed_, slot.coord, *slot.chunk, baseline));
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
bool World::edit(WorldPosition position, std::uint8_t bits) {
    if (!bits || (bits & ~generator_.edit_bits)) throw std::invalid_argument("Unknown tile edit bits");
    position.move({});
    for (auto& slot : slots_)
        if (slot.state.load(std::memory_order_acquire) == State::active && slot.coord == position.chunk) {
            const auto index = static_cast<std::size_t>(position.local.y) * chunk_side +
                               static_cast<std::size_t>(position.local.x);
            generator_.apply_edit(generator_.context, slot.chunk->tiles[index], bits);
            slot.chunk->changes[index] |= bits;
            slot.chunk->dirty = true;
            return true;
        }
    return false;
}
} // namespace seed
