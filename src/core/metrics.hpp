#pragma once
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace seed {
using MeasurementClock = std::chrono::steady_clock;
inline double milliseconds(MeasurementClock::time_point begin, MeasurementClock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}
struct Distribution {
    std::size_t count{};
    double mean{}, minimum{}, p50{}, p95{}, p99{}, maximum{};
};
// Allocation happens once at setup; overflow is an error rather than a hidden reallocation.
class Samples {
public:
    explicit Samples(std::size_t capacity) : capacity_(capacity) { values_.reserve(capacity); }
    void add(double value) {
        if (!std::isfinite(value) || value < 0) throw std::invalid_argument("Invalid timing sample");
        if (values_.size() == capacity_) throw std::overflow_error("Timing sample capacity exceeded");
        values_.push_back(value);
    }
    Distribution summarize() const {
        if (values_.empty()) return {};
        auto sorted = values_;
        std::sort(sorted.begin(), sorted.end());
        double sum = 0;
        for (const auto value : sorted)
            sum += value;
        const auto percentile = [&](double p) {
            return sorted[static_cast<std::size_t>(std::ceil(p * sorted.size())) - 1];
        };
        return {sorted.size(),    sum / sorted.size(), sorted.front(), percentile(0.5),
                percentile(0.95), percentile(0.99),    sorted.back()};
    }

private:
    std::size_t capacity_;
    std::vector<double> values_;
};
inline void json_string(std::ostream& out, std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    out << '"';
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\')
            out << '\\' << static_cast<char>(c);
        else if (c < 32)
            out << "\\u00" << hex[c >> 4] << hex[c & 15];
        else
            out << static_cast<char>(c);
    }
    out << '"';
}
inline void write_distribution(std::ostream& out, const Samples& samples) {
    const auto s = samples.summarize();
    // No samples means unavailable, never a fabricated zero-duration measurement.
    if (!s.count) {
        out << "null";
        return;
    }
    out << "{\"count\":" << s.count << ",\"mean\":" << s.mean << ",\"min\":" << s.minimum
        << ",\"p50\":" << s.p50 << ",\"p95\":" << s.p95 << ",\"p99\":" << s.p99 << ",\"max\":" << s.maximum
        << '}';
}
std::uint64_t peak_resident_bytes();
} // namespace seed
