#include "mge/perf/frame_stats.h"
#include <algorithm>
#include <cstdio>
#include <unistd.h>

namespace mge::perf {

FrameProfiler::FrameProfiler(uint32_t capacity) : ring_(capacity ? capacity : 1) {
    scratch_.reserve(ring_.size());
}

void FrameProfiler::record(const FrameSample& s) {
    ring_[head_] = s;
    head_ = (head_ + 1) % uint32_t(ring_.size());
    if (count_ < ring_.size()) ++count_;
    ++total_;
}

void FrameProfiler::reset() {
    head_ = count_ = 0;
    total_ = 0;
}

FrameSample FrameProfiler::recent(uint32_t i) const {
    if (i >= count_) return {};
    const uint32_t n = uint32_t(ring_.size());
    return ring_[(head_ + n - 1 - i) % n];
}

Percentiles FrameProfiler::stats(Section s) const {
    Percentiles p;
    if (count_ == 0) return p;
    scratch_.clear();
    double sum = 0;
    for (uint32_t i = 0; i < count_; ++i) {
        const FrameSample& f = recent(i);
        float v = 0;
        switch (s) {
        case Section::Frame: v = f.frameMs; break;
        case Section::Cpu: v = f.cpuMs; break;
        case Section::Physics: v = f.physicsMs; break;
        case Section::Batch: v = f.batchMs; break;
        case Section::Render: v = f.renderMs; break;
        }
        scratch_.push_back(v);
        sum += v;
    }
    std::sort(scratch_.begin(), scratch_.end());
    // Nearest-rank percentiles.
    auto rank = [&](float q) {
        size_t k = size_t(q * float(scratch_.size()) + 0.999f);
        k = k == 0 ? 0 : k - 1;
        return scratch_[std::min(k, scratch_.size() - 1)];
    };
    p.avg = float(sum / double(count_));
    p.p50 = rank(0.50f);
    p.p95 = rank(0.95f);
    p.p99 = rank(0.99f);
    p.max = scratch_.back();
    return p;
}

uint32_t FrameProfiler::framesOver(float budgetMs) const {
    uint32_t n = 0;
    for (uint32_t i = 0; i < count_; ++i) n += recent(i).frameMs > budgetMs ? 1 : 0;
    return n;
}

MemoryUsage processMemory() {
    MemoryUsage m;
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return m;
    unsigned long long size = 0, resident = 0;
    if (std::fscanf(f, "%llu %llu", &size, &resident) == 2) {
        const long page = sysconf(_SC_PAGESIZE);
        m.virtualBytes = size * uint64_t(page > 0 ? page : 4096);
        m.rssBytes = resident * uint64_t(page > 0 ? page : 4096);
    }
    std::fclose(f);
    return m;
}

namespace {
double kib(uint64_t b) { return double(b) / 1024.0; }
double mib(uint64_t b) { return double(b) / (1024.0 * 1024.0); }
}  // namespace

std::string formatReport(const BenchReport& r) {
    char buf[1024];
    std::snprintf(buf, sizeof buf,
                  "%s %ux%u | %.1f fps over %.1f s | frame avg %.2f p50 %.2f p95 %.2f p99 %.2f max %.2f ms "
                  "(%u over %.1f ms) | cpu avg %.2f p95 %.2f | physics %.2f | batch %.2f | render %.2f ms | "
                  "draws %u, sprites %u (+%u culled), tiles %u | textures %u (%.1f KiB) | "
                  "entities %u, bodies %u (%u awake), contacts %u | mem ecs %.1f KiB, physics %.1f KiB, rss %.1f MiB",
                  r.backend, r.surfaceW, r.surfaceH, r.fps, r.seconds, double(r.frame.avg), double(r.frame.p50),
                  double(r.frame.p95), double(r.frame.p99), double(r.frame.max), r.framesOverBudget,
                  double(r.budgetMs), double(r.cpu.avg), double(r.cpu.p95), double(r.physics.avg),
                  double(r.batch.avg), double(r.render.avg), r.drawCalls, r.spritesDrawn, r.spritesCulled,
                  r.tilesDrawn, r.textures, kib(r.textureBytes), r.entities, r.bodies, r.awakeBodies, r.contacts,
                  kib(r.ecsBytes), kib(r.physicsBytes), mib(r.rssBytes));
    return buf;
}

std::string toJson(const BenchReport& r) {
    auto pct = [](const Percentiles& p) {
        char b[160];
        std::snprintf(b, sizeof b, "{\"avg\":%.3f,\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f,\"max\":%.3f}", double(p.avg),
                      double(p.p50), double(p.p95), double(p.p99), double(p.max));
        return std::string(b);
    };
    char head[512];
    std::snprintf(head, sizeof head,
                  "{\"backend\":\"%s\",\"surface\":[%u,%u],\"seconds\":%.3f,\"frames\":%llu,\"fps\":%.2f,"
                  "\"budgetMs\":%.3f,\"framesOverBudget\":%u,",
                  r.backend, r.surfaceW, r.surfaceH, r.seconds, (unsigned long long)r.frames, r.fps,
                  double(r.budgetMs), r.framesOverBudget);
    char tail[640];
    std::snprintf(tail, sizeof tail,
                  "\"drawCalls\":%u,\"spritesDrawn\":%u,\"spritesCulled\":%u,\"tilesDrawn\":%u,\"textures\":%u,"
                  "\"textureBytes\":%llu,\"entities\":%u,\"bodies\":%u,\"awakeBodies\":%u,\"contacts\":%u,"
                  "\"ecsBytes\":%llu,\"physicsBytes\":%llu,\"rssBytes\":%llu}",
                  r.drawCalls, r.spritesDrawn, r.spritesCulled, r.tilesDrawn, r.textures,
                  (unsigned long long)r.textureBytes, r.entities, r.bodies, r.awakeBodies, r.contacts,
                  (unsigned long long)r.ecsBytes, (unsigned long long)r.physicsBytes, (unsigned long long)r.rssBytes);
    return std::string(head) + "\"frameMs\":" + pct(r.frame) + ",\"cpuMs\":" + pct(r.cpu) + ",\"physicsMs\":" +
           pct(r.physics) + ",\"batchMs\":" + pct(r.batch) + ",\"renderMs\":" + pct(r.render) + "," + tail;
}

}  // namespace mge::perf
