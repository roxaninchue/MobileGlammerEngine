#pragma once
// Frame timing and benchmark reports (M1 #20).
//  - FrameProfiler keeps the last N frame samples in a ring (no allocation after construction)
//    and answers average / p50 / p95 / p99 / max for any timed section, plus jank counts.
//  - processMemory() reads the process's resident and virtual size (/proc/self/statm, which
//    Android exposes too).
//  - BenchReport gathers everything a run reports (timing, draw calls, sprites, textures,
//    ECS / physics / process memory) and prints as one log line or as JSON.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mge::perf {

struct FrameSample {
    float frameMs = 0;     // whole frame, including the wait for the display (vsync)
    float cpuMs = 0;       // engine work: update + physics + batching + command recording
    float physicsMs = 0;
    float batchMs = 0;
    float renderMs = 0;    // renderer submit (GPU work is not visible here)
};

enum class Section : uint8_t { Frame, Cpu, Physics, Batch, Render };

struct Percentiles {
    float avg = 0, p50 = 0, p95 = 0, p99 = 0, max = 0;
};

class FrameProfiler {
public:
    explicit FrameProfiler(uint32_t capacity = 600);
    void record(const FrameSample& s);
    void reset();
    uint32_t count() const { return count_; }
    uint32_t capacity() const { return uint32_t(ring_.size()); }
    uint64_t totalFrames() const { return total_; }

    Percentiles stats(Section s) const;
    // Frames whose frameMs exceeded the budget (e.g. 16.7 for 60 Hz).
    uint32_t framesOver(float budgetMs) const;
    // i = 0 is the newest sample. i >= count() returns an empty sample.
    FrameSample recent(uint32_t i) const;

private:
    std::vector<FrameSample> ring_;
    mutable std::vector<float> scratch_;
    uint32_t head_ = 0, count_ = 0;
    uint64_t total_ = 0;
};

struct MemoryUsage {
    uint64_t rssBytes = 0;
    uint64_t virtualBytes = 0;
};
MemoryUsage processMemory();

struct BenchReport {
    const char* backend = "none";
    uint32_t surfaceW = 0, surfaceH = 0;
    double seconds = 0;
    uint64_t frames = 0;
    double fps = 0;
    Percentiles frame, cpu, physics, batch, render;
    uint32_t framesOverBudget = 0;
    float budgetMs = 16.667f;
    uint32_t drawCalls = 0, spritesDrawn = 0, spritesCulled = 0, tilesDrawn = 0;
    uint32_t textures = 0;
    uint64_t textureBytes = 0;
    uint32_t entities = 0, bodies = 0, awakeBodies = 0, contacts = 0;
    uint64_t ecsBytes = 0, physicsBytes = 0, rssBytes = 0;
};

std::string formatReport(const BenchReport& r);   // one line for logcat
std::string toJson(const BenchReport& r);

}  // namespace mge::perf
