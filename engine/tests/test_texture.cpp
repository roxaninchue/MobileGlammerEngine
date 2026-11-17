#include "test_main.h"
#include "mge/texture.h"
#include <cstring>
#include <map>
#include <string>
using namespace mge;
using namespace mge::assets;

namespace {
void put32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }

std::vector<uint8_t> makeAstc(uint32_t w, uint32_t h, uint8_t block) {
    std::vector<uint8_t> b = {0x13, 0xAB, 0xA1, 0x5C, block, block, 1};
    for (uint32_t v : {w, h, 1u}) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v >> 16)); }
    size_t n = textureLevelBytes(block == 4 ? TextureFormat::ASTC_4x4 : TextureFormat::ASTC_8x8, w, h);
    for (size_t i = 0; i < n; ++i) b.push_back(uint8_t(i));
    return b;
}

std::vector<uint8_t> makeKtxEtc2(uint32_t w, uint32_t h, uint32_t mips) {
    std::vector<uint8_t> b = {0xAB, 'K', 'T', 'X', ' ', '1', '1', 0xBB, '\r', '\n', 0x1A, '\n'};
    for (uint32_t v : {0x04030201u, 0u, 1u, 0u, 0x9278u, 0x1908u, w, h, 0u, 0u, 1u, mips, 8u}) put32(b, v);
    for (int i = 0; i < 8; ++i) b.push_back(0);   // key/value data
    for (uint32_t m = 0; m < mips; ++m) {
        uint32_t mw = w >> m ? w >> m : 1, mh = h >> m ? h >> m : 1;
        uint32_t n = uint32_t(textureLevelBytes(TextureFormat::ETC2_RGBA8, mw, mh));
        put32(b, n);
        for (uint32_t i = 0; i < n; ++i) b.push_back(uint8_t(m));
    }
    return b;
}

struct MemFs { std::map<std::string, std::vector<uint8_t>> files; };
bool memReader(const char* path, std::vector<uint8_t>& out, void* user) {
    auto& fs = *static_cast<MemFs*>(user);
    auto it = fs.files.find(path);
    if (it == fs.files.end()) return false;
    out = it->second;
    return true;
}
}  // namespace

TEST(texture_parse_astc) {
    auto f = makeAstc(100, 50, 8);
    TextureData t;
    CHECK(parseTexture(f.data(), f.size(), t));
    CHECK(t.format == TextureFormat::ASTC_8x8 && t.width == 100 && t.height == 50 && t.mipCount == 1);
    CHECK(t.bytes.size() == 13 * 7 * 16);
    CHECK(!parseTexture(f.data(), f.size() - 1, t));   // truncated
}

TEST(texture_parse_ktx_mips) {
    auto f = makeKtxEtc2(64, 32, 7);
    TextureData t;
    CHECK(parseTexture(f.data(), f.size(), t));
    CHECK(t.format == TextureFormat::ETC2_RGBA8 && t.mipCount == 7);
    CHECK(t.mips[0].size == 16 * 8 * 16 && t.mips[6].width == 1 && t.mips[6].height == 1 && t.mips[6].size == 16);
    CHECK(t.bytes[t.mips[3].offset] == 3);
    for (size_t cut : {size_t(10), size_t(70), f.size() - 1}) CHECK(!parseTexture(f.data(), cut, t));
    auto bad = f;
    bad[28] = 0x01;   // unknown format
    CHECK(!parseTexture(bad.data(), bad.size(), t));
}

TEST(texture_cache_async_and_dedupe) {
    jobs::init();
    MemFs fs;
    fs.files["a.astc"] = makeAstc(64, 64, 4);
    fs.files["b.ktx"] = makeKtxEtc2(32, 32, 1);
    TextureCache cache({64 * MiB, memReader, &fs});
    TextureHandle a = cache.acquire("a.astc");
    TextureHandle a2 = cache.acquire("a.astc");
    TextureHandle b = cache.acquire("b.ktx");
    TextureHandle missing = cache.acquire("nope.ktx");
    CHECK(a == a2 && cache.refCount(a) == 2 && cache.count() == 3);
    cache.waitAll();
    CHECK(cache.state(a) == TextureCache::State::Ready && cache.get(a)->width == 64);
    CHECK(cache.get(b)->format == TextureFormat::ETC2_RGBA8);
    CHECK(cache.state(missing) == TextureCache::State::Failed && cache.get(missing) == nullptr);
    cache.release(missing);
    cache.update();
    CHECK(!cache.alive(missing) && cache.count() == 2);
}

TEST(texture_cache_budget_lru) {
    jobs::init();
    MemFs fs;
    for (int i = 0; i < 4; ++i) fs.files["t" + std::to_string(i)] = makeAstc(128, 128, 4);   // 16 KiB each
    TextureCache cache({40 * KiB, memReader, &fs});
    TextureHandle h[4];
    for (int i = 0; i < 4; ++i) h[i] = cache.acquire("t" + std::to_string(i));
    cache.waitAll();
    CHECK(cache.residentBytes() == 64 * KiB);
    cache.update();
    CHECK(cache.residentBytes() == 64 * KiB);          // all referenced: nothing evicted
    cache.release(h[2]); cache.release(h[0]); cache.release(h[1]);
    cache.update();                                     // evict oldest unreferenced until <= 40 KiB
    CHECK(!cache.alive(h[0]) && !cache.alive(h[1]) && cache.alive(h[2]) && cache.alive(h[3]));
    CHECK(cache.residentBytes() == 32 * KiB);
    TextureHandle again = cache.acquire("t0");          // reload into a recycled slot
    CHECK(!(again == h[0]) && cache.get(h[0]) == nullptr);
    cache.waitAll();
    CHECK(cache.get(again) != nullptr);
}
