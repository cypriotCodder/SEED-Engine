#include "core/metrics.hpp"
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
} // namespace seed
