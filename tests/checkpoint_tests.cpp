#include "core/metrics.hpp"
#include "io/binary.hpp"
#include "io/checkpoint.hpp"
#include "io/storage.hpp"
#include "world/player_save.hpp"
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

namespace {
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class F>
void rejects(F&& fn) {
    bool rejected = false;
    try {
        fn();
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected, "Expected failure was accepted");
}
void state(const std::filesystem::path& path, std::uint8_t value) {
    for (const auto* name : {"world.seed", "0_0.chunk", "player.delta"})
        seed::write_blob(path / name, std::span(&value, 1));
}
void verify(const std::filesystem::path& path, std::uint8_t value) {
    for (const auto* name : {"world.seed", "0_0.chunk", "player.delta"})
        check(seed::read_blob(path / name) == std::vector<std::uint8_t>{value}, "Mixed checkpoint ages");
}
std::filesystem::path latest(const std::filesystem::path& root);
std::atomic<bool> frozen{false}, resume_publication{false};
void pause_publication(seed::Checkpoint::Stage stage) {
    if (stage != seed::Checkpoint::Stage::files_written) return;
    frozen.store(true);
    while (!resume_publication.load())
        std::this_thread::yield();
}
void async_snapshot(const std::filesystem::path& root) {
    seed::Jobs jobs(1);
    {
        seed::Checkpoint save(root);
        state(save.working_directory(), 10);
        save.commit();
        state(save.working_directory(), 11);
        struct Resume {
            ~Resume() { resume_publication.store(true); }
        } resume;
        check(save.begin_commit(jobs, pause_publication), "Background commit did not start");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!frozen.load()) {
            if (std::chrono::steady_clock::now() > deadline)
                throw std::runtime_error("Worker did not reach barrier");
            std::this_thread::yield();
        }
        check(!save.begin_commit(jobs), "Overlapping publication was accepted");
        rejects([&] { (void)save.metrics(); });
        state(save.working_directory(), 12);
        verify(latest(root), 10);
        resume_publication.store(true);
        save.finish_commit();
        verify(latest(root), 11); // The new working state must not leak into the frozen checkpoint.
        verify(save.working_directory(), 12);
        const auto metrics = save.metrics();
        check(metrics.linked_files + metrics.copied_files + metrics.moved_files == 3,
              "Snapshot accounting mismatch");
        save.commit();
        verify(latest(root), 12);
        const auto copied = root / "raw-copy";
        check(!seed::snapshot_blob(save.read_path("world.seed"), copied, false), "Copy path linked");
        check(!std::filesystem::equivalent(copied, save.read_path("world.seed")), "Copy shares inode");
        check(seed::read_blob(copied) == std::vector<std::uint8_t>{12}, "Raw copy changed payload");
    }
    seed::Checkpoint reopened(root);
    verify(reopened.working_directory(), 12);
}
seed::Checkpoint::Stage interruption{};
void interrupt(seed::Checkpoint::Stage stage) {
    if (stage == interruption) throw std::runtime_error("Injected interrupted commit");
}
void crash(seed::Checkpoint::Stage stage) {
    if (stage == seed::Checkpoint::Stage::recovery_written) std::_Exit(73);
}
std::filesystem::path latest(const std::filesystem::path& root) {
    const auto data = seed::read_blob(root / "CURRENT");
    seed::Reader r(data);
    r.u32();
    r.u32();
    return root / "checkpoints" / std::to_string(r.u64());
}
} // namespace
// A save can hold several worlds: a game's scenes keep their files under "<scene>." names, and a
// save with only scene worlds commits and reopens.
void scene_files(const std::filesystem::path& root) {
    {
        seed::Checkpoint save(root);
        for (const char* name :
             {"cave.world.seed", "cave.-3_12.chunk", "cave.player.delta", "level_2.0_0.chunk"})
            check(save.read_path(name).filename() == name, "Scene save names are accepted");
        for (const char* name :
             {"bad name.world.seed", "a.b.world.seed", ".world.seed", "cave.notes.txt", "cave."})
            rejects([&] { save.read_path(name); });
        seed::write_blob(save.working_directory() / "cave.world.seed", std::vector<std::uint8_t>{1, 2});
        seed::write_blob(save.working_directory() / "cave.0_0.chunk", std::vector<std::uint8_t>{3});
        save.commit();
    }
    seed::Checkpoint reopened(root);
    check(std::filesystem::exists(reopened.read_path("cave.world.seed")) &&
              std::filesystem::exists(reopened.read_path("cave.0_0.chunk")),
          "A save of scene worlds reopens");
}

int main(int argc, char** argv) {
    try {
        if (argc < 2) throw std::runtime_error("Expected private test directory");
        const std::filesystem::path root(argv[1]);
        if (argc == 3) {
            seed::Checkpoint save(root);
            state(save.working_directory(), 9);
            save.commit(crash);
            throw std::runtime_error("Crash hook did not execute");
        }
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        scene_files(root / "scenes");
        const auto legacy = root / "legacy-flat";
        std::filesystem::create_directories(legacy);
        state(legacy, 1);
        rejects([&] { seed::Checkpoint old(legacy); });
        verify(legacy, 1);
        {
            seed::Checkpoint save(root);
            state(save.working_directory(), 1);
            rejects([&] { seed::Checkpoint other(root); });
            save.commit();
            state(save.working_directory(), 2);
            save.commit();
        }
        verify(legacy, 1);
        for (const auto stage :
             {seed::Checkpoint::Stage::files_written, seed::Checkpoint::Stage::manifest_written,
              seed::Checkpoint::Stage::recovery_written, seed::Checkpoint::Stage::published}) {
            {
                seed::Checkpoint save(root);
                state(save.working_directory(), 3);
                interruption = stage;
                rejects([&] { save.commit(interrupt); });
            }
            seed::Checkpoint reopened(root);
            verify(reopened.working_directory(), stage == seed::Checkpoint::Stage::published ? 3 : 2);
        }
        // A real process exit skips destructors and releases the OS writer lock.
        std::string command = "\"" + std::string(argv[0]) + "\" \"" + root.string() + "\" crash";
        check(std::system(command.c_str()) != 0, "Crash child unexpectedly succeeded");
        {
            seed::Checkpoint save(root);
            verify(save.working_directory(), 3);
            state(save.working_directory(), 4);
            save.commit();
        }
        // Replacing a payload with a valid but wrong envelope must fail its manifest CRC.
        state(latest(root), 5);
        {
            seed::Checkpoint save(root);
            check(save.recovered(), "Corrupt checkpoint did not report recovery");
            verify(save.working_directory(), 3);
            state(save.working_directory(), 6);
            save.commit();
        }
        std::ofstream(root / "CURRENT", std::ios::binary | std::ios::trunc) << "broken";
        {
            seed::Checkpoint save(root);
            check(save.recovered(), "Corrupt pointer did not report recovery");
            verify(save.working_directory(), 3);
            state(save.working_directory(), 7);
            save.commit();
        }
        unsigned retained = 0;
        for (const auto& entry : std::filesystem::directory_iterator(root / "checkpoints"))
            if (entry.is_directory()) ++retained;
        check(retained == 2, "Checkpoint retention is not bounded");
        // An unknown version is not corruption and must never silently roll back.
        auto data = seed::read_blob(root / "CURRENT");
        data[4] = 2;
        seed::write_blob(root / "CURRENT", data);
        rejects([&] { seed::Checkpoint save(root); });
        verify(legacy, 1);
        async_snapshot(root / "async");
        {
            seed::Jobs jobs(1);
            seed::Checkpoint save(root / "async");
            state(save.working_directory(), 13);
            interruption = seed::Checkpoint::Stage::manifest_written;
            check(save.begin_commit(jobs, interrupt), "Error test job did not start");
            rejects([&] { save.finish_commit(); });
            verify(latest(root / "async"), 12);
            rejects([&] { save.commit(); });
        }
        {
            seed::Checkpoint save(root / "player-return");
            state(save.working_directory(), 1);
            seed::save_player(save.working_directory(), 123, 1, {{}, {12, 12}});
            save.commit();
            seed::save_player(save.working_directory(), 123, 1, {},
                              std::filesystem::exists(save.read_path("player.delta")));
            save.commit();
            const auto position = seed::load_player(save.read_path("player.delta").parent_path(), 123, 1);
            check(position.local.x == 0 && position.local.y == 0,
                  "Returning to spawn resurrected old player state");
        }
        seed::Samples samples(100);
        for (unsigned i = 1; i <= 100; ++i)
            samples.add(i);
        const auto summary = samples.summarize();
        check(summary.p50 == 50 && summary.p95 == 95 && summary.p99 == 99 && summary.mean == 50.5,
              "Nearest-rank statistics are wrong");
        rejects([&] { samples.add(1); });
        seed::Samples empty(1);
        rejects([&] { empty.add(-1); });
        std::ostringstream json;
        seed::write_distribution(json, empty);
        check(json.str() == "null", "Missing samples reported as zero measurements");
        std::cout << "Checkpoint recovery, interrupted commits, process crash, locking, legacy rejection and "
                     "metrics passed.\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
