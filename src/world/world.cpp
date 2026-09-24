#include "world/world.hpp"
#include "io/binary.hpp"
#include "io/storage.hpp"

namespace seed {
World::World(Jobs& jobs, std::uint64_t seed, std::filesystem::path directory)
    : jobs_(jobs), seed_(seed), directory_(std::move(directory)) {
    std::filesystem::create_directories(directory_);
    const auto metadata = directory_ / "world.seed";
    if (std::filesystem::exists(metadata)) {
        const auto bytes = read_blob(metadata);
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
    for (auto& slot : slots_) {
        slot.world = this;
        slot.chunk = &pool_.get(pool_.create());
    }
}
World::~World() {
    jobs_.wait();
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
        generate(*slot.chunk, slot.world->seed_, slot.coord);
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
    const auto file = path(slot.coord);
    if (!std::filesystem::exists(file)) return;
    const auto bytes = read_blob(file);
    Reader input(bytes);
    if (input.u32() != 0x4b4e4843 || input.u32() != 1 || input.u32() != generator_version ||
        input.u64() != seed_ || input.u64() != std::uint64_t(slot.coord.x) ||
        input.u64() != std::uint64_t(slot.coord.y))
        throw std::runtime_error("Invalid chunk header");
    const auto count = input.u16();
    if (count > slot.chunk->tiles.size()) throw std::runtime_error("Invalid chunk change count");
    for (unsigned i = 0; i < count; ++i) {
        const auto index = input.u16();
        const auto change = input.u8();
        if (index >= slot.chunk->tiles.size() || change < 1 || change > 3 || slot.chunk->changes[index])
            throw std::runtime_error("Invalid or duplicate chunk change");
        slot.chunk->changes[index] = change;
        slot.chunk->tiles[index].tree = false;
        if (change & 2) {
            slot.chunk->tiles[index].material = 0;
            slot.chunk->tiles[index].elevation = -0.1F;
        }
    }
    if (!input.done()) throw std::runtime_error("Trailing chunk data");
}
void World::write(Slot& slot) {
    if (!slot.chunk->dirty) return;
    Bytes output;
    output.u32(0x4b4e4843);
    output.u32(1);
    output.u32(generator_version);
    output.u64(seed_);
    output.u64(std::uint64_t(slot.coord.x));
    output.u64(std::uint64_t(slot.coord.y));
    std::uint16_t count = 0;
    for (auto change : slot.chunk->changes)
        if (change) ++count;
    output.u16(count);
    for (std::size_t i = 0; i < slot.chunk->changes.size(); ++i)
        if (slot.chunk->changes[i]) {
            output.u16(static_cast<std::uint16_t>(i));
            output.u8(slot.chunk->changes[i]);
        }
    write_blob(path(slot.coord), output.data);
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
            slot.state = nearby(slot.coord, center, 3) ? State::active : State::empty;
            state = slot.state.load();
        }
        if (state == State::active && !nearby(slot.coord, center, 3)) {
            if (slot.chunk->dirty) {
                slot.state = State::saving;
                jobs_.submit({save_job, &slot});
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
                jobs_.submit({generate_job, empty});
            }
        }
}
void World::settle(ChunkCoord center) {
    stream(center);
    jobs_.wait();
    stream(center);
    jobs_.wait();
    stream(center);
}
void World::save() {
    jobs_.wait();
    check_errors();
    for (auto& slot : slots_)
        if (slot.state.load() == State::active || slot.state.load() == State::ready) write(slot);
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
