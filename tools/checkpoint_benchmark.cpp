#include "io/checkpoint.hpp"
#include "io/storage.hpp"
#include <charconv>
#include <chrono>
#include <cstdio>
#include <string_view>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
} // namespace
int main(int argc, char** argv) {
    try {
        unsigned files = 1000;
        if (argc < 2 || argc > 3)
            throw std::invalid_argument("Usage: seed_checkpoint_bench NEW_DIRECTORY [CHUNK_FILES]");
        if (argc == 3) {
            const std::string_view value(argv[2]);
            const auto result = std::from_chars(value.data(), value.data() + value.size(), files);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || !files ||
                files > 99999)
                throw std::invalid_argument("Chunk file count must be 1..99999");
        }
        const std::filesystem::path root(argv[1]);
        if (std::filesystem::exists(root)) throw std::invalid_argument("Benchmark directory must be new");
        std::vector<std::uint8_t> data(4096);
        std::uint32_t random = 123;
        for (auto& byte : data) {
            random = random * 1664525 + 1013904223;
            byte = static_cast<std::uint8_t>(random >> 24);
        }
        seed::Jobs jobs(1);
        std::printf("{\"chunk_files\":%u,\"payload_bytes_per_file\":4096,\"commits\":[", files);
        {
            seed::Checkpoint checkpoint(root);
            const auto& work = checkpoint.working_directory();
            // Opaque storage fixtures, not a playable world. Setup cost is excluded from measurements.
            seed::write_blob(work / "world.seed", data);
            for (unsigned i = 0; i < files; ++i)
                seed::write_blob(work / (std::to_string(i) + "_0.chunk"), data);
            for (unsigned run = 0; run < 3; ++run) {
                data[0] = static_cast<std::uint8_t>(run);
                seed::write_blob(work / "0_0.chunk", data);
                const auto start = Clock::now();
                if (!checkpoint.begin_commit(jobs)) throw std::logic_error("Unexpected pending commit");
                const auto capture_ms = elapsed(start);
                checkpoint.finish_commit();
                const auto total_ms = elapsed(start);
                const auto m = checkpoint.metrics();
                std::printf("%s{\"capture_ms\":%.6f,\"total_ms\":%.6f,\"publication_ms\":%.6f,"
                            "\"linked_files\":%zu,\"copied_files\":%zu,\"moved_files\":%zu}",
                            run ? "," : "", capture_ms, total_ms, m.publication_ms, m.linked_files,
                            m.copied_files, m.moved_files);
            }
        }
        const auto start = Clock::now();
        seed::Checkpoint reopen(root);
        std::printf("],\"startup_ms\":%.6f}\n", elapsed(start));
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Checkpoint benchmark: %s\n", error.what());
        return 1;
    }
}
