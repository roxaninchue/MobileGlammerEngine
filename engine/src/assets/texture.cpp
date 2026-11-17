#include "mge/texture.h"
#include "mge/log.h"
#include <cstdio>
#include <cstring>

namespace mge::assets {
namespace {

struct BlockInfo { uint32_t bw, bh, bytes; };
BlockInfo blockInfo(TextureFormat f) {
    switch (f) {
        case TextureFormat::RGBA8: return {1, 1, 4};
        case TextureFormat::ETC2_RGB8: return {4, 4, 8};
        case TextureFormat::ETC2_RGBA8: return {4, 4, 16};
        case TextureFormat::ASTC_4x4: return {4, 4, 16};
        case TextureFormat::ASTC_6x6: return {6, 6, 16};
        case TextureFormat::ASTC_8x8: return {8, 8, 16};
    }
    return {1, 1, 4};
}

uint32_t rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
uint32_t rd24(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16; }

constexpr uint32_t kMaxDim = 16384;

}  // namespace

const char* formatName(TextureFormat f) {
    static const char* n[] = {"RGBA8", "ETC2_RGB8", "ETC2_RGBA8", "ASTC_4x4", "ASTC_6x6", "ASTC_8x8"};
    return n[int(f)];
}

size_t textureLevelBytes(TextureFormat f, uint32_t w, uint32_t h) {
    BlockInfo b = blockInfo(f);
    return size_t((w + b.bw - 1) / b.bw) * ((h + b.bh - 1) / b.bh) * b.bytes;
}

// ARM .astc: magic 13 AB A1 5C, block x/y/z (1 byte each), size x/y/z (3 bytes LE each).
bool parseAstc(const uint8_t* d, size_t size, TextureData& out) {
    if (size < 16 || rd32(d) != 0x5CA1AB13u) return false;
    uint32_t bx = d[4], by = d[5], bz = d[6];
    uint32_t w = rd24(d + 7), h = rd24(d + 10), depth = rd24(d + 13);
    if (bz != 1 || depth != 1 || w == 0 || h == 0 || w > kMaxDim || h > kMaxDim) return false;
    TextureFormat f;
    if (bx == 4 && by == 4) f = TextureFormat::ASTC_4x4;
    else if (bx == 6 && by == 6) f = TextureFormat::ASTC_6x6;
    else if (bx == 8 && by == 8) f = TextureFormat::ASTC_8x8;
    else { MGE_LOGW("astc: unsupported block %ux%u", bx, by); return false; }
    size_t bytes = textureLevelBytes(f, w, h);
    if (size - 16 < bytes) return false;
    out = TextureData{};
    out.format = f; out.width = w; out.height = h; out.mipCount = 1;
    out.mips[0] = {w, h, 0, uint32_t(bytes)};
    out.bytes.assign(d + 16, d + 16 + bytes);
    return true;
}

// KTX 1.1, little-endian, 2D only.
bool parseKtx(const uint8_t* d, size_t size, TextureData& out) {
    static const uint8_t id[12] = {0xAB, 'K', 'T', 'X', ' ', '1', '1', 0xBB, '\r', '\n', 0x1A, '\n'};
    if (size < 64 || std::memcmp(d, id, 12) != 0) return false;
    if (rd32(d + 12) != 0x04030201u) { MGE_LOGW("ktx: big-endian files not supported"); return false; }
    uint32_t glInternal = rd32(d + 28);
    uint32_t w = rd32(d + 36), h = rd32(d + 40), depth = rd32(d + 44);
    uint32_t arrays = rd32(d + 48), faces = rd32(d + 52), mips = rd32(d + 56), kv = rd32(d + 60);
    if (w == 0 || w > kMaxDim || h > kMaxDim || depth > 1 || arrays > 1 || faces != 1) return false;
    if (h == 0) return false;   // 1D textures unsupported
    if (mips == 0) mips = 1;
    if (mips > kMaxMips) return false;
    TextureFormat f;
    switch (glInternal) {
        case 0x8058: case 0x1908: f = TextureFormat::RGBA8; break;          // GL_RGBA8 / GL_RGBA
        case 0x9274: f = TextureFormat::ETC2_RGB8; break;
        case 0x9278: f = TextureFormat::ETC2_RGBA8; break;
        case 0x93B0: f = TextureFormat::ASTC_4x4; break;
        case 0x93B4: f = TextureFormat::ASTC_6x6; break;
        case 0x93B7: f = TextureFormat::ASTC_8x8; break;
        default: MGE_LOGW("ktx: unsupported glInternalFormat 0x%x", glInternal); return false;
    }
    size_t off = 64;
    if (kv > size - off) return false;
    off += kv;
    out = TextureData{};
    out.format = f; out.width = w; out.height = h; out.mipCount = mips;
    for (uint32_t m = 0; m < mips; ++m) {
        uint32_t mw = w >> m ? w >> m : 1, mh = h >> m ? h >> m : 1;
        if (size - off < 4) return false;
        uint32_t imageSize = rd32(d + off);
        off += 4;
        size_t expect = textureLevelBytes(f, mw, mh);
        if (imageSize != expect || size - off < imageSize) return false;
        out.mips[m] = {mw, mh, uint32_t(out.bytes.size()), imageSize};
        out.bytes.insert(out.bytes.end(), d + off, d + off + imageSize);
        off += (imageSize + 3u) & ~3u;   // mipPadding
        if (off > size) off = size;
    }
    return true;
}

bool parseTexture(const uint8_t* d, size_t size, TextureData& out) {
    if (size >= 4 && rd32(d) == 0x5CA1AB13u) return parseAstc(d, size, out);
    if (size >= 1 && d[0] == 0xAB) return parseKtx(d, size, out);
    return false;
}

bool stdioReader(const char* path, std::vector<uint8_t>& out, void*) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    bool ok = n >= 0;
    if (ok) { out.resize(size_t(n)); ok = std::fread(out.data(), 1, out.size(), f) == out.size(); }
    std::fclose(f);
    return ok;
}

// ------------------------------------------------------------------ cache
TextureCache::~TextureCache() { waitAll(); }

TextureCache::Slot* TextureCache::slot(TextureHandle h) const {
    if (h.index >= slots_.size()) return nullptr;
    Slot* s = slots_[h.index].get();
    return s->used && s->generation == h.generation ? s : nullptr;
}

void TextureCache::loadJob(void* p) {
    auto* s = static_cast<Slot*>(p);
    std::vector<uint8_t> file;
    TextureData data;
    bool ok = s->owner->cfg_.reader(s->path.c_str(), file, s->owner->cfg_.user) &&
              parseTexture(file.data(), file.size(), data);
    if (ok) s->data = std::move(data);
    else MGE_LOGW("texture: failed to load '%s'", s->path.c_str());
    s->state.store(ok ? State::Ready : State::Failed, std::memory_order_release);
}

TextureHandle TextureCache::acquire(std::string_view path) {
    ++tick_;
    std::string key(path);
    auto it = byPath_.find(key);
    if (it != byPath_.end()) {
        Slot* s = slots_[it->second].get();
        ++s->refs;
        s->lastUsed = tick_;
        return {it->second, s->generation};
    }
    uint32_t idx;
    if (!free_.empty()) { idx = free_.back(); free_.pop_back(); }
    else { idx = uint32_t(slots_.size()); slots_.push_back(std::make_unique<Slot>()); }
    Slot* s = slots_[idx].get();
    s->path = std::move(key);
    s->refs = 1;
    s->lastUsed = tick_;
    s->used = true;
    s->owner = this;
    s->data = TextureData{};
    s->state.store(State::Loading, std::memory_order_relaxed);
    byPath_.emplace(s->path, idx);
    jobs::submit(&TextureCache::loadJob, s, &pending_);
    return {idx, s->generation};
}

void TextureCache::addRef(TextureHandle h) { if (Slot* s = slot(h)) ++s->refs; }
void TextureCache::release(TextureHandle h) {
    Slot* s = slot(h);
    MGE_ASSERT(!s || s->refs > 0, "texture released too many times");
    if (s && s->refs) --s->refs;
}

bool TextureCache::alive(TextureHandle h) const { return slot(h) != nullptr; }
TextureCache::State TextureCache::state(TextureHandle h) const {
    Slot* s = slot(h);
    return s ? s->state.load(std::memory_order_acquire) : State::Failed;
}
const TextureData* TextureCache::get(TextureHandle h) const {
    Slot* s = slot(h);
    return s && s->state.load(std::memory_order_acquire) == State::Ready ? &s->data : nullptr;
}
uint32_t TextureCache::refCount(TextureHandle h) const { Slot* s = slot(h); return s ? s->refs : 0; }

size_t TextureCache::residentBytes() const {
    size_t b = 0;
    for (auto& s : slots_)
        if (s->used && s->state.load(std::memory_order_acquire) == State::Ready) b += s->data.bytes.size();
    return b;
}

void TextureCache::evict(uint32_t index) {
    Slot* s = slots_[index].get();
    byPath_.erase(s->path);
    s->used = false;
    ++s->generation;   // outstanding handles go stale
    s->data = TextureData{};
    s->path.clear();
    free_.push_back(index);
}

void TextureCache::update() {
    // Failed loads with no users are dropped so a retry can happen later.
    for (uint32_t i = 0; i < slots_.size(); ++i) {
        Slot* s = slots_[i].get();
        if (s->used && s->refs == 0 && s->state.load(std::memory_order_acquire) == State::Failed) evict(i);
    }
    size_t resident = residentBytes();
    while (resident > cfg_.budgetBytes) {
        uint32_t victim = UINT32_MAX;
        uint64_t oldest = UINT64_MAX;
        for (uint32_t i = 0; i < slots_.size(); ++i) {
            Slot* s = slots_[i].get();
            if (s->used && s->refs == 0 && s->state.load(std::memory_order_acquire) == State::Ready && s->lastUsed < oldest) {
                oldest = s->lastUsed;
                victim = i;
            }
        }
        if (victim == UINT32_MAX) break;   // everything left is in use: over budget is reported, not forced
        resident -= slots_[victim]->data.bytes.size();
        evict(victim);
    }
}

}  // namespace mge::assets
