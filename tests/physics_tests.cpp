#include "physics/physics.hpp"
#include "test_world.hpp"
#include "world/chunk_file.hpp"
#include "world/world.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

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
constexpr std::size_t header_only_size = 4 + 4 + 8 + 4 + 8 + 8 + 8 + 2 + 2 + 2 + 2 + 2 + 4;

std::unique_ptr<seed::Chunk> generated(seed::ChunkCoord coord) {
    auto chunk = std::make_unique<seed::Chunk>();
    seed::fill_chunk(test_world::generator(), seed, coord, *chunk);
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
    const auto empty = seed::encode_chunk(test_world::generator(), seed, origin, *baseline, baseline->bodies);
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
    const auto bytes = seed::encode_chunk(test_world::generator(), seed, origin, *edited, baseline->bodies);

    auto loaded = generated(origin);
    seed::decode_chunk(bytes, test_world::generator(), seed, origin, *loaded);
    check(loaded->changes[33] == 3 && loaded->tiles[33].elevation < 0, "Tile delta round trip");
    check(loaded->bodies.count == bodies.count && loaded->bodies.broken == bodies.broken,
          "Body counts round trip");
    for (std::size_t i = 0; i < bodies.count; ++i)
        check(seed::encode_body(loaded->bodies.bodies[i]) == seed::encode_body(bodies.bodies[i]),
              "Body state round trip");

    auto decode_into_fresh = [&](std::vector<std::uint8_t> data) {
        auto target = generated(origin);
        seed::decode_chunk(data, test_world::generator(), seed, origin, *target);
    };
    auto trailing = bytes;
    trailing.push_back(0);
    rejects([&] { decode_into_fresh(trailing); }, "Trailing byte accepted");
    rejects([&] { decode_into_fresh({bytes.begin(), bytes.end() - 1}); }, "Truncated file accepted");
    rejects(
        [&] {
            auto target = generated({1, 0});
            seed::decode_chunk(bytes, test_world::generator(), seed, {1, 0}, *target);
        },
        "File for another coordinate accepted");
    auto old_version = bytes;
    old_version[4] = 4;
    rejects([&] { decode_into_fresh(old_version); }, "Version 4 chunk file accepted");
    auto other_generator = bytes;
    other_generator[8] ^= 1; // Low byte of the generator ID.
    rejects([&] { decode_into_fresh(other_generator); }, "Chunk file from another generator accepted");

    // Offsets: 44-byte header up to the tile count, 3 bytes per tile record, then the
    // replaced-tile count.
    const std::size_t body_count_offset = 4 + 4 + 8 + 4 + 8 + 8 + 8 + 2 + 3 + 2;
    const std::size_t first_record = body_count_offset + 2;
    auto duplicate = bytes;
    duplicate[first_record + 2 + seed::body_record_size] = duplicate[first_record]; // Second ID = first ID.
    duplicate[first_record + 3 + seed::body_record_size] = duplicate[first_record + 1];
    rejects([&] { decode_into_fresh(duplicate); }, "Duplicate body ID accepted");
    auto far_away = bytes;
    far_away[first_record + 3] = 9; // Position chunk x of the first record: 9 chunks from its owner.
    rejects([&] { decode_into_fresh(far_away); }, "Body outside owner reach accepted");
}

// Replaced tiles (World::set_tile) are saved whole and restored over the generated, edited tile.
void replaced_tiles() {
    const seed::ChunkCoord origin{};
    auto generator = test_world::generator();
    generator.materials = 4;
    auto baseline = generated(origin);
    auto edited = generated(origin);
    const seed::Tile replacement{0.75F, 2, seed::tile_solid, seed::tile_object(3), {1, 2, 3, 4}};
    edited->changes[40] = 1; // Edited, then replaced: the replacement wins.
    edited->tiles[40] = replacement;
    edited->replaced.set(40);
    edited->tiles[41] = replacement;
    edited->replaced.set(41);
    auto replaced_only = generated(origin);
    replaced_only->replaced.set(7);
    check(!seed::chunk_matches_baseline(*replaced_only, baseline->bodies),
          "Replaced tiles make a chunk differ");
    const auto bytes = seed::encode_chunk(generator, seed, origin, *edited, baseline->bodies);

    auto loaded = generated(origin);
    seed::decode_chunk(bytes, generator, seed, origin, *loaded);
    const auto same = [&](const seed::Tile& a) {
        return a.elevation == replacement.elevation && a.material == replacement.material &&
               a.flags == replacement.flags && a.object == replacement.object && a.game == replacement.game;
    };
    check(same(loaded->tiles[40]) && same(loaded->tiles[41]) && loaded->changes[40] == 1 &&
              loaded->replaced == edited->replaced && !loaded->replaced[42],
          "Replaced tiles round trip whole");

    auto decode_into_fresh = [&](std::vector<std::uint8_t> data) {
        auto target = generated(origin);
        seed::decode_chunk(data, generator, seed, origin, *target);
    };
    // After the 44-byte header and the tile count: one 3-byte tile record, the replaced count, then 13-byte
    // records of u16 index, f32 elevation, material, flags, object and four game bytes.
    const std::size_t first = 44 + 2 + 3 + 2;
    auto unregistered = bytes;
    unregistered[first + 6] = 4;
    rejects([&] { decode_into_fresh(unregistered); }, "Unregistered replaced material accepted");
    auto bad_object = bytes;
    bad_object[first + 8] = 5;
    rejects([&] { decode_into_fresh(bad_object); }, "Unregistered replaced object accepted");
    auto infinite = bytes;
    infinite[first + 4] = 0x80;
    infinite[first + 5] = 0x7f;
    rejects([&] { decode_into_fresh(infinite); }, "Non-finite replaced elevation accepted");
    auto out_of_order = bytes;
    out_of_order[first + 13] = 40; // The second record repeats the first index.
    rejects([&] { decode_into_fresh(out_of_order); }, "Repeated replaced tile accepted");

    auto invalid = generated(origin);
    invalid->tiles[3].material = 9;
    invalid->replaced.set(3);
    rejects<std::invalid_argument>(
        [&] { seed::encode_chunk(generator, seed, origin, *invalid, baseline->bodies); },
        "An unregistered material was saved");
}

// Saved entities travel verbatim in the chunk file and are validated on load.
void entity_codec() {
    const seed::ChunkCoord origin{};
    auto baseline = generated(origin);
    auto edited = generated(origin);
    const std::uint8_t payload[] = {7, 0, 42};
    seed::append_entity(edited->entities, {{{}, {40.5F, 3.25F}}, 0.5F, 2, {0.4F, 0.6F}, payload});
    seed::append_entity(edited->entities, {{{}, {1, 1}}, 0, 0, {1, 1}, {}});
    check(!seed::chunk_matches_baseline(*edited, baseline->bodies), "Entities make a chunk differ");
    const auto bytes = seed::encode_chunk(test_world::generator(), seed, origin, *edited, baseline->bodies);

    auto loaded = generated(origin);
    seed::decode_chunk(bytes, test_world::generator(), seed, origin, *loaded);
    check(loaded->entities == edited->entities, "Entity records round trip");
    std::vector<seed::EntityRecord> records;
    std::vector<std::vector<std::uint8_t>> payloads;
    seed::each_entity(loaded->entities, [&](const seed::EntityRecord& record) {
        records.push_back(record);
        payloads.emplace_back(record.payload.begin(), record.payload.end());
    });
    check(records.size() == 2, "Both entities decoded");
    // The first record was stored canonically: 40.5 local x is 8.5 into the next chunk.
    check(records[0].position.chunk == seed::ChunkCoord{1, 0} && records[0].position.local.x == 8.5F &&
              records[0].angle == 0.5F && records[0].material == 2 && records[0].size.y == 0.6F,
          "Entity transform and visual round trip");
    check(payloads[0] == std::vector<std::uint8_t>{7, 0, 42} && payloads[1].empty(), "Entity payloads");

    auto decode_into_fresh = [&](std::vector<std::uint8_t> data) {
        auto target = generated(origin);
        seed::decode_chunk(data, test_world::generator(), seed, origin, *target);
    };
    const std::size_t count_offset = header_only_size - 6;
    auto extra = bytes;
    extra[count_offset] = 3; // Claims a third record the section does not hold.
    rejects([&] { decode_into_fresh(extra); }, "Entity count beyond the section accepted");
    auto short_section = bytes;
    short_section[count_offset + 2] -= 1; // Section length one byte short of its records.
    rejects([&] { decode_into_fresh(short_section); }, "Truncated entity section accepted");
    auto bad_float = bytes;
    const std::size_t angle = count_offset + 6 + 24;
    bad_float[angle + 2] = 0x80;
    bad_float[angle + 3] = 0x7f; // +infinity in the first record's angle.
    rejects([&] { decode_into_fresh(bad_float); }, "Non-finite entity angle accepted");

    const std::vector<std::uint8_t> huge(seed::entity_payload_capacity + 1);
    rejects<std::length_error>([&] { seed::append_entity(edited->entities, {{}, 0, 0, {1, 1}, huge}); },
                               "Oversized entity payload accepted");
    check(edited->entities.count == 2, "A rejected record leaves the section unchanged");
}

// world.seed binds a save to one game, generator, generator version and seed.
void save_identity() {
    seed::Jobs jobs(1);
    const auto dir = std::filesystem::temp_directory_path() / "seed-engine-identity-test";
    std::filesystem::remove_all(dir);
    const auto game = seed::stable_id("test-game");
    auto generator = test_world::generator();
    { seed::World created(jobs, game, generator, 7, dir); }
    { seed::World reopened(jobs, game, generator, 7, dir); }
    rejects([&] { seed::World w(jobs, seed::stable_id("other-game"), generator, 7, dir); },
            "Other game accepted");
    rejects([&] { seed::World w(jobs, game, generator, 8, dir); }, "Other seed accepted");
    auto newer = generator;
    newer.version = 2;
    rejects([&] { seed::World w(jobs, game, newer, 7, dir); }, "Other generator version accepted");
    auto renamed = generator;
    renamed.name = "another-generator";
    rejects([&] { seed::World w(jobs, game, renamed, 7, dir); }, "Other generator accepted");
    std::filesystem::remove_all(dir);
    check(seed::stable_id("seed-demo") != seed::stable_id("seed-demp"), "Stable IDs distinguish names");
}

// Attaches empty chunks around `center` (skipping chunks already resident via `skip`), so that
// bodies owned by `center` are eligible for simulation.
void attach_neighbours(seed::Physics& physics, seed::ChunkCoord center,
                       seed::ChunkCoord skip = {INT64_MAX, 0}) {
    static const auto empty = std::make_unique<seed::ChunkBodies>();
    for (std::int64_t dy = -1; dy <= 1; ++dy)
        for (std::int64_t dx = -1; dx <= 1; ++dx) {
            const seed::ChunkCoord coord{center.x + dx, center.y + dy};
            if ((dx || dy) && !(coord == skip)) physics.attach(coord, *empty);
        }
}

std::unique_ptr<seed::ChunkBodies> single_body(seed::ChunkCoord owner, seed::Vec2 local, float inverse_mass) {
    auto bodies = std::make_unique<seed::ChunkBodies>();
    seed::BodyState body;
    body.position = body.previous = {owner, local};
    body.inverse_mass = inverse_mass;
    bodies->bodies[0] = body;
    bodies->count = bodies->recipe_count = 1;
    return bodies;
}

void residency() {
    seed::Jobs jobs(2);
    seed::Scene scene;
    seed::Physics physics(scene, jobs);
    const seed::ChunkCoord origin{};
    auto chunk = generated(origin);
    const auto baseline = generated(origin);

    attach_neighbours(physics, origin);
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

    for (std::uint16_t pier = 0; pier < 4; ++pier)
        check(physics.destroy(origin, pier), "Destroy a pier");
    check(!physics.destroy(origin, 0) && !physics.destroy(origin, 99), "Destroy rejects missing bodies");
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
    const auto bytes = seed::encode_chunk(test_world::generator(), seed, origin, *chunk, baseline->bodies);
    auto reloaded = generated(origin);
    seed::decode_chunk(bytes, test_world::generator(), seed, origin, *reloaded);
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
// Regression (Codex review 1): destroyed built blocks must return their pool slot at once.
void built_slots_are_reclaimed() {
    seed::Jobs jobs(1);
    seed::Scene scene;
    seed::Physics physics(scene, jobs);
    const seed::ChunkCoord owner{8, 8};
    auto bodies = std::make_unique<seed::ChunkBodies>();
    physics.attach(owner, *bodies);
    for (std::size_t i = 0; i < seed::Physics::body_capacity + 100; ++i) {
        check(physics.build({owner, {12, 12}}), "Build must succeed while the chunk has no blocks");
        check(physics.damage({owner, {12, 12}}, 100), "Destroy the block");
    }
    check(physics.count() == 0 && physics.build({owner, {12, 12}}), "Pool is not exhausted by churn");
    check(physics.release(owner, *bodies) && bodies->count == 1, "The surviving block is saved");
}

// Regression (Codex review 2): a simulated body must collide with a resident neighbour whose owner
// is outside the simulation radius, and must not move at all while a neighbour is missing.
void boundary_collisions() {
    seed::Jobs jobs(1);
    seed::Scene scene;
    seed::Physics physics(scene, jobs);
    const seed::ChunkCoord mover_owner{1, 0}, wall_owner{2, 0};
    auto mover = single_body(mover_owner, {31.8F, 12}, 1);
    auto wall = single_body(wall_owner, {0.2F, 12}, 0);
    physics.attach(mover_owner, *mover);
    attach_neighbours(physics, mover_owner, wall_owner);
    const seed::WorldPosition anchor{{0, 0}, {16, 16}};

    // The wall's chunk is not resident yet: the mover must wait rather than simulate blind.
    physics.step(anchor);
    physics.release(mover_owner, *mover);
    check(mover->bodies[0].position.local.x == 31.8F, "Body simulated before its neighbours loaded");
    physics.attach(mover_owner, *mover);

    physics.attach(wall_owner, *wall);
    for (int i = 0; i < 30; ++i)
        physics.step(anchor);
    physics.release(mover_owner, *mover);
    physics.release(wall_owner, *wall);
    const auto gap = seed::relative(wall->bodies[0].position, mover->bodies[0].position).x;
    check(gap > 0.95F, "Mover must be pushed out of the static neighbour");
    check(wall->bodies[0].position.local.x == 0.2F, "An unsimulated neighbour must not move");
}

// Regression (Codex review 3): every state reader rejects access while a step runs.
void readers_reject_pending_step() {
    seed::Jobs jobs(1);
    seed::Scene scene;
    seed::Physics physics(scene, jobs);
    physics.begin_step({});
    rejects<std::logic_error>([&] { (void)physics.count(); }, "count() during a step");
    rejects<std::logic_error>([&] { (void)physics.unsupported(); }, "unsupported() during a step");
    rejects<std::logic_error>([&] { (void)physics.grounded_unsupported(); },
                              "grounded_unsupported() during a step");
    physics.finish_step();
}

// Regression (Codex review 4): a rejected attach changes nothing and can be retried.
void failed_attach_is_clean() {
    seed::Jobs jobs(1);
    seed::Scene scene;
    seed::Physics physics(scene, jobs);
    auto full = std::make_unique<seed::ChunkBodies>();
    for (std::uint16_t i = 0; i < 255; ++i) {
        full->bodies[i].position =
            full->bodies[i].previous = {{}, {1 + static_cast<float>(i % 30), 1 + static_cast<float>(i / 30)}};
    }
    full->count = full->recipe_count = 255;
    for (std::int64_t c = 0; c < 16; ++c) {
        for (std::uint16_t i = 0; i < 255; ++i) {
            full->bodies[i].position.chunk = full->bodies[i].previous.chunk = {c + 100, 0};
        }
        physics.attach({c + 100, 0}, *full);
    }
    auto rest = single_body({200, 0}, {5, 5}, 1);
    auto fifteen = std::make_unique<seed::ChunkBodies>(*rest);
    for (std::uint16_t i = 1; i < 15; ++i)
        fifteen->bodies[i] = fifteen->bodies[0];
    fifteen->count = fifteen->recipe_count = 15;
    physics.attach({200, 0}, *fifteen);
    check(physics.count() == seed::Physics::body_capacity - 1, "Pool holds 4,095 bodies");

    auto two = single_body({300, 0}, {5, 5}, 1);
    two->bodies[1] = two->bodies[0];
    two->count = two->recipe_count = 2;
    rejects([&] { physics.attach({300, 0}, *two); }, "Over-budget attach accepted");
    check(physics.count() == seed::Physics::body_capacity - 1, "Rejected attach allocated bodies");
    physics.release({200, 0}, *fifteen);
    physics.attach({300, 0}, *two);
    check(physics.count() == seed::Physics::body_capacity - 14, "Retry succeeds after capacity frees up");
}
} // namespace

int main() {
    try {
        job_groups();
        codec();
        entity_codec();
        replaced_tiles();
        residency();
        save_identity();
        built_slots_are_reclaimed();
        boundary_collisions();
        readers_reject_pending_step();
        failed_attach_is_clean();
        std::cout << "Job group, chunk codec and physics residency checks passed.\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
