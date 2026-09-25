#include "core/metrics.hpp"
#include <fstream>
#include <iomanip>
#ifdef _WIN32
#define NOMINMAX
#include <psapi.h>
#include <windows.h>
#else
#include <sys/resource.h>
#endif
namespace seed {
std::uint64_t peak_resident_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS info{};
    info.cb = sizeof(info);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof(info)))
        throw std::runtime_error("Cannot measure process memory");
    return static_cast<std::uint64_t>(info.PeakWorkingSetSize);
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) throw std::runtime_error("Cannot measure process memory");
#ifdef __APPLE__
    return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
#endif
#endif
}
void BenchmarkReport::write(const std::filesystem::path& path, const BenchmarkMetadata& info,
                            const Samples& gpu_samples, unsigned gpu_skipped) const {
    std::ofstream report(path);
    report << std::setprecision(9);
    report << "{\n\"schema\":1,\"seed\":\"" << info.seed << "\",\"workload\":\""
           << (info.stream_workload ? "stream" : "static") << "\",\"warmup_frames\":" << info.warmup_frames
           << ",\"measured_frames\":" << info.measured_frames << ",\"width\":" << info.width
           << ",\"height\":" << info.height << ",\"damage_demo\":" << (info.damage_demo ? "true" : "false")
           << ",\"overview\":" << (info.overview ? "true" : "false") << ",\"platform\":";
    seed::json_string(report, info.platform);
    report << ",\"generator_version\":" << info.generator_version;
    report << ",\"compiler\":";
#ifdef _MSC_VER
    seed::json_string(report, "MSVC " + std::to_string(_MSC_VER));
#else
    seed::json_string(report, __VERSION__);
#endif
    report << ",\"gpu\":";
    seed::json_string(report, info.gpu);
    report << ",\"opengl\":";
    seed::json_string(report, info.opengl);
#ifdef NDEBUG
    report << ",\"build\":\"release\"";
#else
    report << ",\"build\":\"debug\"";
#endif
    report << ",\n\"milliseconds\":{\"frame_cpu\":";
    seed::write_distribution(report, frame_times);
    report << ",\"update_cpu\":";
    seed::write_distribution(report, update_times);
    report << ",\"stream_cpu_subset_of_update\":";
    seed::write_distribution(report, stream_times);
    report << ",\"render_cpu\":";
    seed::write_distribution(report, render_times);
    report << ",\"present_cpu\":";
    seed::write_distribution(report, present_times);
    report << ",\"render_gpu\":";
    seed::write_distribution(report, gpu_samples);
    report << ",\"previous_physics_step_worker\":";
    seed::write_distribution(report, physics_times);
    report << ",\"physics_join_and_scene_sync_cpu_subset_of_update\":";
    seed::write_distribution(report, physics_join_times);
    report << "},\n\"draw_calls\":";
    seed::write_distribution(report, draws);
    report << ",\"resident_bodies_at_frame_start\":";
    seed::write_distribution(report, bodies);
    report << ",\"gpu_samples_skipped\":" << gpu_skipped
           << ",\"session_generated_chunks\":" << info.generated_chunks
           << ",\"session_generation_total_ms\":" << info.generation_ns / 1000000.0
           << ",\"session_generation_max_ms\":" << info.generation_max_ns / 1000000.0
           << ",\"shutdown_checkpoint_ms\":" << info.save_ms
           << ",\"process_peak_resident_bytes\":" << seed::peak_resident_bytes();
    std::uintmax_t save_bytes = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(info.save_directory))
        if (entry.is_regular_file()) save_bytes += entry.file_size();
    report << ",\"save_directory_bytes\":" << save_bytes << "}\n";
    report.close();
    if (!report) throw std::runtime_error("Cannot write benchmark report");
}
} // namespace seed
