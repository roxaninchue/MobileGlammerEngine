#pragma once
// Texture assets: GPU-native compressed formats only, so nothing is decoded on the CPU.
//  - .astc (ARM ASTC header) and .ktx (KTX 1.1: ETC2, ASTC, RGBA8, with mips).
//  - TextureCache: refcounted generational handles, async loads on the job system,
//    LRU eviction of unreferenced textures when over the memory budget.
// GPU upload is the renderer's job (M1 #14); this layer owns the bytes.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "mge/jobs.h"
#include "mge/memory.h"

namespace mge::assets {

enum class TextureFormat : uint8_t { RGBA8, ETC2_RGB8, ETC2_RGBA8, ASTC_4x4, ASTC_6x6, ASTC_8x8 };
const char* formatName(TextureFormat f);
size_t textureLevelBytes(TextureFormat f, uint32_t width, uint32_t height);

constexpr uint32_t kMaxMips = 16;
struct MipLevel { uint32_t width, height, offset, size; };

struct TextureData {
    TextureFormat format = TextureFormat::RGBA8;
    uint32_t width = 0, height = 0, mipCount = 0;
    MipLevel mips[kMaxMips] = {};
    std::vector<uint8_t> bytes;   // mips packed back to back, no padding
};

bool parseAstc(const uint8_t* data, size_t size, TextureData& out);
bool parseKtx(const uint8_t* data, size_t size, TextureData& out);
bool parseTexture(const uint8_t* data, size_t size, TextureData& out);   // sniffs the format

struct TextureHandle {
    uint32_t index = UINT32_MAX, generation = 0;
    bool valid() const { return index != UINT32_MAX; }
    bool operator==(const TextureHandle& o) const { return index == o.index && generation == o.generation; }
};

// Reads a whole file. Runs on job workers, so it must be thread-safe.
// Android passes an AAssetManager reader; host tests pass an in-memory one.
using FileReader = bool (*)(const char* path, std::vector<uint8_t>& out, void* user);
bool stdioReader(const char* path, std::vector<uint8_t>& out, void* user);

struct TextureCacheConfig {
    size_t budgetBytes = 64 * MiB;
    FileReader reader = stdioReader;
    void* user = nullptr;
};

class TextureCache {
public:
    enum class State : uint8_t { Loading, Ready, Failed };
    using Config = TextureCacheConfig;

    TextureCache() : cfg_() {}
    explicit TextureCache(const Config& cfg) : cfg_(cfg) {}
    ~TextureCache();
    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;

    // Returns a handle with one reference; starts an async load the first time a path is seen.
    TextureHandle acquire(std::string_view path);
    void addRef(TextureHandle h);
    void release(TextureHandle h);

    bool alive(TextureHandle h) const;
    State state(TextureHandle h) const;
    const TextureData* get(TextureHandle h) const;   // null until Ready
    uint32_t refCount(TextureHandle h) const;

    // Call once per frame on the owning thread: evicts unreferenced textures, oldest first,
    // while resident bytes exceed the budget.
    void update();
    void waitAll() { jobs::wait(pending_); }

    size_t residentBytes() const;
    size_t count() const { return byPath_.size(); }

private:
    struct Slot {
        std::string path;
        uint32_t generation = 0;
        uint32_t refs = 0;
        uint64_t lastUsed = 0;
        bool used = false;
        std::atomic<State> state{State::Loading};
        TextureData data;
        TextureCache* owner = nullptr;
    };
    static void loadJob(void* p);
    Slot* slot(TextureHandle h) const;
    void evict(uint32_t index);

    Config cfg_;
    std::vector<std::unique_ptr<Slot>> slots_;   // Slot addresses stay stable for in-flight jobs
    std::vector<uint32_t> free_;
    std::unordered_map<std::string, uint32_t> byPath_;
    jobs::Counter pending_;
    uint64_t tick_ = 0;
};

}  // namespace mge::assets
