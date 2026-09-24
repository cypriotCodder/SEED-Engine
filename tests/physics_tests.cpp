#include "physics/physics.hpp"
#include "world/chunk_file.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Exception = std::runtime_error, class F>
void rejects(F&& f, const char* message) {
    try {
        f();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(message);
}

constexpr std::uint64_t seed = 77;
constexpr std::size_t header_only_size = 4 + 4 + 4 + 8 + 8 + 8 + 2 + 2 + 2;

std::unique_ptr<seed::Chunk> generated(seed::ChunkCoord coord) {
    auto chunk = std::make_unique<seed::Chunk>();
    seed::generate(*chunk, seed, coord);
    return chunk;
}

void job_groups() {
    seed::Jobs jobs(2);
    seed::JobGroup slow, fast;
    std::atomic<bool> release{false}, ran{false};
    struct Context {
        std::atomic<bool>* flag;
    };
    Context blocker{&release}, marker{&ran};
    jobs.submit({[](void* c) noexcept {
                     while (!static_cast<Context*>(c)->flag->load())
                         std::this_thread::yield();
                 },
                 &blocker, &slow});
    jobs.submit({[](void* c) noexcept { static_cast<Context*>(c)->flag->store(true); }, &marker, &fast});
    // Must return even though the slow group is still blocked.
    jobs.wait(fast);
    check(ran.load() && jobs.busy(slow), "Group wait must not wait for other groups");
    release = true;
    jobs.wait(slow);
    check(!jobs.busy(slow), "Group wait completes its own jobs");
}

void codec() {
    const seed::ChunkCoord origin{};
    auto baseline = generated(origin);
    check(baseline->bodies.recipe_count == 19 && baseline->bodies.joint_count > 0, "Demo platform recipe");
    check(seed::chunk_matches_baseline(*baseline, baseline->bodies), "Fresh chunk matches its baseline");
    const auto empty = seed::encode_chunk(seed, origin, *baseline, baseline->bodies);
    check(empty.size() == header_only_size, "Untouched chunk encodes no records");

    // Change terrain, move one recipe body, destroy another, break a joint and add a built body.
    auto edited = generated(origin);
    auto& bodies = edited->bodies;
    edited->changes[33] = 3;
    bodies.bodies[5].position.move({0.25F, -0.5F});
    bodies.bodies[5].angle = 0.3F;
    bodies.bodies[6].exists = false;
    bodies.bodies[6].health = 0;
    bodies.broken.set(2);
    seed::BodyState built;
    built.position = built.previous = {{}, {12, 12}};
    built.half = {0.45F, 0.45F};
    bodies.bodies[bodies.count++] = built;
    check(!seed::chunk_matches_baseline(*edited, baseline->bodies), "Edited chunk differs from baseline");
    const auto bytes = seed::encode_chunk(seed, origin, *edited, baseline->bodies);

    auto loaded = generated(origin);
    seed::decode_chunk(bytes, seed, origin, *loaded);
    check(loaded->changes[33] == 3 && loaded->tiles[33].elevation < 0, "Tile delta round trip");
    check(loaded->bodies.count == bodies.count && loaded->bodies.broken == bodies.broken,
          "Body counts round trip");
    for (std::size_t i = 0; i < bodies.count; ++i)
        check(seed::encode_body(loaded->bodies.bodies[i]) == seed::encode_body(bodies.bodies[i]),
              "Body state round trip");

    auto decode_into_fresh = [&](std::vector<std::uint8_t> data) {
        auto target = generated(origin);
        seed::decode_chunk(data, seed, origin, *target);
    };
    auto trailing = bytes;
    trailing.push_back(0);
    rejects([&] { decode_into_fresh(trailing); }, "Trailing byte accepted");
    rejects([&] { decode_into_fresh({bytes.begin(), bytes.end() - 1}); }, "Truncated file accepted");
    rejects(
        [&] {
            auto target = generated({1, 0});
            seed::decode_chunk(bytes, seed, {1, 0}, *target);
        },
        "File for another coordinate accepted");
    auto old_version = bytes;
    old_version[4] = 1;
    rejects([&] { decode_into_fresh(old_version); }, "Version 1 chunk file accepted");

    // Offsets: 42-byte header prefix up to the tile count, then 3 bytes per tile record.
    const std::size_t body_count_offset = 4 + 4 + 4 + 8 + 8 + 8 + 2 + 3;
    const std::size_t first_record = body_count_offset + 2;
    auto duplicate = bytes;
    duplicate[first_record + 2 + seed::body_record_size] = duplicate[first_record]; // Second ID = first ID.
    duplicate[first_record + 3 + seed::body_record_size] = duplicate[first_record + 1];
    rejects([&] { decode_into_fresh(duplicate); }, "Duplicate body ID accepted");
    auto far_away = bytes;
    far_away[first_record + 3] = 9; // Position chunk x of the first record: 9 chunks from its owner.
    rejects([&] { decode_into_fresh(far_away); }, "Body outside owner reach accepted");
}

void legacy_codec() {
    auto chunk = generated({});
    seed::Bytes old;
    old.u32(0x59444f42);
    old.u32(2);
    old.u32(seed::generator_version);
    old.u64(seed);
    old.u16(21);
    old.u16(3);
    auto removed = chunk->bodies.bodies[0];
    removed.exists = false;
    removed.health = 0;
    auto built = chunk->bodies.bodies[4];
    built.position = built.previous = {{}, {12, 12}};
    built.height = built.previous_height = 0;
    for (const auto& item : {std::pair{std::uint16_t{0}, removed}, std::pair{std::uint16_t{19}, removed},
                             std::pair{std::uint16_t{20}, built}}) {
        old.u16(item.first);
        const auto body = seed::encode_body(item.second);
        old.data.insert(old.data.end(), body.begin(), body.end());
    }
    old.u16(1);
    old.u16(0);
    seed::decode_legacy_bodies(old.data, seed, *chunk);
    check(!chunk->bodies.bodies[0].exists && chunk->bodies.count == 20 &&
              chunk->bodies.bodies[19].position.local.x == 12 && chunk->bodies.broken[0],
          "Legacy building migration lost destroyed, built or joint state");
    auto fresh = generated({});
    auto invalid = old.data;
    invalid[4] = 99;
    rejects([&] { seed::decode_legacy_bodies(invalid, seed, *fresh); }, "Unknown legacy version accepted");
    seed::Bytes terrain;
    terrain.u32(seed::chunk_file_magic);
    terrain.u32(1);
    terrain.u32(seed::generator_version);
    terrain.u64(seed);
    terrain.u64(0);
    terrain.u64(0);
    terrain.u16(1);
    terrain.u16(0);
    terrain.u8(3);
    seed::decode_chunk(terrain.data, seed, {}, *fresh);
    check(fresh->changes[0] == 3 && fresh->tiles[0].elevation < 0, "Legacy terrain migration lost edits");
}

void residency() {
    seed::Jobs jobs(2);
    seed::Scene scene;
    seed::Physics physics(scene, jobs);
    const seed::ChunkCoord origin{};
    auto chunk = generated(origin);
    const auto baseline = generated(origin);

    physics.attach(origin, chunk->bodies);
    check(physics.count() == 19, "All recipe bodies attached");
    for (int i = 0; i < 60; ++i)
        physics.step({origin, {0, 5}});
    check(physics.unsupported() == 0, "Intact platform is supported");
    check(!physics.store(origin, chunk->bodies), "An untouched platform must not report changes");

    physics.begin_step({origin, {0, 5}});
    rejects<std::logic_error>([&] { (void)physics.find(origin, 0); },
                              "Access during a running step must throw");
    physics.finish_step();

    physics.collapse_demo();
    for (int i = 0; i < 60; ++i)
        physics.step({origin, {0, 5}});
    check(physics.unsupported() == 15 && physics.grounded_unsupported() == 15, "Collapse grounds 15 pieces");
    check(physics.build({origin, {12, 12}}), "Build a block");
    check(!physics.build({origin, {12, 12}}), "Cannot build inside another block");

    check(physics.release(origin, chunk->bodies), "Collapse is reported as a change");
    check(physics.count() == 0 && !physics.find(origin, 5), "Release removes the chunk's bodies");
    check(chunk->bodies.count == 20 && !chunk->bodies.bodies[0].exists && chunk->bodies.bodies[19].exists,
          "Released state holds destroyed piers and the built block");

    // Persist, reload into a fresh chunk, and resume.
    const auto bytes = seed::encode_chunk(seed, origin, *chunk, baseline->bodies);
    auto reloaded = generated(origin);
    seed::decode_chunk(bytes, seed, origin, *reloaded);
    physics.attach(origin, reloaded->bodies);
    check(physics.count() == 16, "Reattached 15 fallen pieces and one built block");
    physics.step({origin, {0, 5}});
    check(physics.unsupported() == 16 && physics.grounded_unsupported() == 16,
          "Collapsed state survives reload");
    check(!physics.store(origin, reloaded->bodies), "Settled reloaded state reports no change");

    // Destroying the built block drops it from the next save.
    check(physics.damage({origin, {12, 12}}, 100), "Damage the built block");
    check(physics.store(origin, reloaded->bodies) && reloaded->bodies.count == 19,
          "Destroyed built body is dropped");
    physics.release(origin, reloaded->bodies);

    // Damage hits the piece whose centre is nearest, whatever the pool order. Pier 0 overlaps a
    // side beam's hit margin; releasing and reattaching must not change which one is struck.
    auto fresh = generated(origin);
    for (int round = 0; round < 2; ++round) {
        physics.attach(origin, fresh->bodies);
        const auto pier = physics.find(origin, 0);
        check(pier && physics.damage(*pier, 100) && !physics.find(origin, 0) && physics.find(origin, 10),
              "Damage strikes the pier, not an overlapping beam");
        physics.release(origin, fresh->bodies);
        fresh = generated(origin);
    }
    check(physics.count() == 0 && scene.transforms.values().empty(),
          "Scene entities are released with their chunk");
}
} // namespace

int main() {
    try {
        job_groups();
        codec();
        legacy_codec();
        residency();
        std::cout << "Job group, chunk codec and physics residency checks passed.\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
