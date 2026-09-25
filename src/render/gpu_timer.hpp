#pragma once
#include "core/metrics.hpp"
#include "render/gl.hpp"
#include <array>

namespace seed {
// Queries are polled without waiting during frames. Only drain() waits, after measurement ends.
class GpuTimer final {
public:
    explicit GpuTimer(std::size_t frames) : samples(frames), enabled_(frames != 0) {
        if (enabled_) gl_.GenQueries(static_cast<GLsizei>(ids_.size()), ids_.data());
    }
    ~GpuTimer() {
        if (enabled_) gl_.DeleteQueries(static_cast<GLsizei>(ids_.size()), ids_.data());
    }
    GpuTimer(const GpuTimer&) = delete;
    GpuTimer& operator=(const GpuTimer&) = delete;
    void begin(bool record) {
        if (!enabled_) return;
        collect(false);
        for (std::size_t i = 0; i < ids_.size(); ++i) {
            if (pending_[i]) continue;
            active_ = i;
            record_[i] = record;
            gl_.BeginQuery(GL_TIME_ELAPSED, ids_[i]);
            return;
        }
        if (record) ++skipped;
    }
    void end() {
        if (active_ == ids_.size()) return;
        gl_.EndQuery(GL_TIME_ELAPSED);
        pending_[active_] = true;
        active_ = ids_.size();
    }
    void drain() {
        collect(true);
        gl_.check();
    }
    const char* renderer() const { return description(GL_RENDERER); }
    const char* version() const { return description(GL_VERSION); }
    Samples samples;
    unsigned skipped{};

private:
    const char* description(GLenum name) const {
        const auto* value = gl_.GetString(name);
        return value ? reinterpret_cast<const char*>(value) : "unknown";
    }
    void collect(bool wait) {
        if (!enabled_) return;
        for (std::size_t i = 0; i < ids_.size(); ++i) {
            if (!pending_[i]) continue;
            GLint available = GL_FALSE;
            if (!wait) gl_.GetQueryObjectiv(ids_[i], GL_QUERY_RESULT_AVAILABLE, &available);
            if (!wait && !available) continue;
            GLuint64 ns{};
            gl_.GetQueryObjectui64v(ids_[i], GL_QUERY_RESULT, &ns);
            if (record_[i]) samples.add(static_cast<double>(ns) / 1000000.0);
            pending_[i] = false;
        }
    }
    Gl gl_;
    std::array<GLuint, 8> ids_{};
    std::array<bool, 8> pending_{}, record_{};
    std::size_t active_ = ids_.size();
    bool enabled_{};
};
} // namespace seed
