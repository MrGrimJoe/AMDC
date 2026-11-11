// Self-contained test suite (no framework). Exit status 0 = all passed.
#include <cstdio>
#include <cstring>
#include <random>
#include <string>

#include "../src/amdc1.hpp"
#include "../src/transforms.hpp"

using namespace amdc1;

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::mt19937 rng(7);
static Bytes rand_bytes(size_t n, int mod = 256) { Bytes b(n); for (auto& c : b) c = uint8_t(rng() % mod); return b; }
static Bytes str(const std::string& s) { return Bytes(s.begin(), s.end()); }

static Bytes rt(const Bytes& d, size_t bs = kDefaultBlock, int level = 1, unsigned th = 1) {
    Bytes blob = compress(d, bs, level, th);
    CHECK(decompress(blob, uint64_t(1) << 32, th) == d);
    return blob;
}

static bool throws(const Bytes& b, uint64_t maxo = uint64_t(1) << 32) {
    try { decompress(b, maxo, 1); } catch (const FormatError&) { return true; }
    return false;
}

static Bytes ppm(int w, int h, int ch, bool comment, const std::string& tail) {
    std::string hdr = std::string(ch == 3 ? "P6" : "P5") + (comment ? "\n# made by test\n" : "\n") +
                      std::to_string(w) + " " + std::to_string(h) + "\n255\n";
    Bytes b = str(hdr);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) for (int c = 0; c < ch; ++c)
        b.push_back(uint8_t(x * 3 + y * 2 + c * 40 + rng() % 4));
    b.insert(b.end(), tail.begin(), tail.end());
    return b;
}

static void test_transforms() {
    for (size_t n : {0, 1, 5, 33, 1000}) {
        Bytes d = rand_bytes(n);
        for (uint32_t k : {1, 2, 4, 9}) {
            Bytes a = xf_apply(XF_DELTA, k, d.data(), n); CHECK(xf_invert(XF_DELTA, k, a.data(), n) == d);
            a = xf_apply(XF_XOR, k, d.data(), n);          CHECK(xf_invert(XF_XOR, k, a.data(), n) == d);
        }
        for (uint32_t w : {2, 3, 4, 8}) {
            Bytes a = xf_apply(XF_SHUFFLE, w, d.data(), n); CHECK(xf_invert(XF_SHUFFLE, w, a.data(), n) == d);
        }
    }
}

static void test_codec() {
    for (uint32_t s : {0u, 1u, 2u, 7u, 64u}) {
        Bytes d = rand_bytes(3000, 8);
        Bytes e = cm_encode(d.data(), d.size(), s);
        CHECK(cm_decode(e.data(), e.size(), d.size(), s) == d);
    }
    for (const Bytes& d : {Bytes{}, Bytes{0}, Bytes{'a'}, Bytes{255, 255}}) rt(d);
    Bytes all; for (int i = 0; i < 256; ++i) all.push_back(uint8_t(i)); rt(all);

    Bytes r = rand_bytes(6000);
    CHECK(rt(r).size() <= r.size() + 20);                     // raw fallback bounds expansion (17 B framing)

    CHECK(rt(Bytes(50000, 0), kDefaultBlock, 0).size() < 100);
    CHECK(rt(Bytes(size_t(1) << 20, 0), kDefaultBlock, 0).size() < 300);     // legit extreme ratio passes the decoder guard
    CHECK(rt(Bytes(size_t(1) << 20, 0xFF), kDefaultBlock, 0).size() < 300);
    std::string t; for (int i = 0; i < 200; ++i) t += "the quick brown fox jumps over the lazy dog. ";
    CHECK(rt(str(t)).size() < t.size() / 20);

    Bytes s; for (uint32_t i = 0; i < 3000; ++i) { uint32_t v = 1000 + i * 7; for (int j = 0; j < 4; ++j) s.push_back(uint8_t(v >> (8 * j))); }
    for (int lvl = 0; lvl <= 2; ++lvl) rt(Bytes(s.begin(), s.begin() + 4000), kDefaultBlock, lvl);
    CHECK(rt(s, 5000).size() < s.size() / 2);                  // multi-block, sampled search
    rt(Bytes(s.begin(), s.begin() + 200), 1);                  // degenerate 1-byte blocks

    Bytes d = rand_bytes(4000, 16);
    CHECK(compress(d, kDefaultBlock, 1, 1) == compress(d, kDefaultBlock, 1, 1));   // deterministic
    Bytes big = rand_bytes(60000, 12);
    CHECK(compress(big, 7000, 1, 1) == compress(big, 7000, 1, 4));                 // thread-count invariant
    CHECK(decompress(compress(big, 7000, 1, 4), uint64_t(1) << 32, 4) == big);
}

static void test_images() {
    for (int ch : {1, 3}) for (const char* tail : {"", "trailing"}) {
        Bytes d = ppm(20, 15, ch, true, tail);
        CHECK(rt(d).size() < d.size());
    }
    rt(ppm(1, 1, 3, false, ""));
    rt(ppm(7, 1, 1, true, ""));
    PnmInfo pi;
    Bytes bad1 = str("P6"), bad2 = str("P6\n2 2\n255\nabc"), bad3 = str("P6\n2 2\n65535\n"); bad3.resize(bad3.size() + 24);
    for (const Bytes& d : {bad1, bad2, bad3}) { CHECK(!parse_pnm(d.data(), d.size(), pi)); rt(d); }
}

static void test_malformed() {
    std::string t; for (int i = 0; i < 40; ++i) t += "hello hello hello hello ";
    Bytes blob = compress(str(t), kDefaultBlock, 1, 1);
    for (size_t i = 0; i < blob.size(); i += std::max<size_t>(1, blob.size() / 40)) {
        // A flip must never yield silently wrong output: it either throws or (when it hits a
        // don't-care byte such as the block-size bound or the coder's flush tail) decodes identically.
        Bytes bad = blob; bad[i] ^= 0x55;
        bool ok;
        try { ok = decompress(bad, uint64_t(1) << 32, 1) == str(t); } catch (const FormatError&) { ok = true; }
        CHECK(ok);
    }
    for (size_t cut : {size_t(0), size_t(3), size_t(8), blob.size() - 1}) CHECK(throws(Bytes(blob.begin(), blob.begin() + cut)));
    Bytes g = blob; g.push_back('x'); CHECK(throws(g));
    Bytes m = blob; m[0] = 'N'; CHECK(throws(m));
    Bytes old = blob; old[4] = 2; CHECK(throws(old));                       // unsupported version (v2 archives are not readable)
    Bytes huge = {'A', 'M', 'D', 'C', 3, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 1, 1, 1, 0, 0, 0, 0, 0};
    CHECK(throws(huge));
    CHECK(throws(blob, 10));
    // implausible expansion: tiny payload claiming a 100 MB block must be refused before any allocation
    Bytes bomb = {'A', 'M', 'D', 'C', 3};
    uint64_t n = 100000000;
    Bytes body;
    auto vi = [&](Bytes& o, uint64_t v) { while (v >= 0x80) { o.push_back(uint8_t(v | 0x80)); v >>= 7; } o.push_back(uint8_t(v)); };
    vi(bomb, n); vi(bomb, 1 << 27);
    bomb.push_back(1); vi(bomb, n); bomb.push_back(0); vi(bomb, 0); bomb.push_back(0); vi(bomb, 4);
    for (int i = 0; i < 8; ++i) bomb.push_back(0);
    CHECK(throws(bomb));
}

static Bytes fake_code(size_t n, int calls) {
    // x86-like stream: opcodes with E8/E9 calls whose relative targets point at a few functions
    Bytes b = rand_bytes(n, 200);
    for (int i = 0; i < calls; ++i) {
        size_t p = rng() % (n - 8);
        uint32_t target = 0x1000 + (rng() % 8) * 0x400;
        uint32_t rel = target - uint32_t(p + 5);
        b[p] = (i & 1) ? 0xE8 : 0xE9;
        b[p + 1] = uint8_t(rel); b[p + 2] = uint8_t(rel >> 8); b[p + 3] = uint8_t(rel >> 16); b[p + 4] = uint8_t(rel >> 24);
    }
    return b;
}

static void test_x86() {
    // Exact round trip on adversarial layouts: overlapping opcodes, trigger bytes inside later windows, all lengths.
    const uint8_t pool[] = {0xE8, 0xE9, 0x00, 0xFF, 0x12, 0xE8};
    for (int round = 0; round < 3000; ++round) {
        size_t n = rng() % 40;
        Bytes d(n);
        for (auto& c : d) c = pool[rng() % 6];
        Bytes t = xf_apply(XF_X86, 0, d.data(), d.size());
        CHECK(t.size() == d.size());
        CHECK(xf_invert(XF_X86, 0, t.data(), t.size()) == d);
    }
    for (int round = 0; round < 40; ++round) {
        Bytes d = rand_bytes(5000, 256);
        for (size_t i = 0; i < d.size(); i += 1 + rng() % 7) d[i] = (rng() & 1) ? 0xE8 : 0xE9;
        for (size_t i = 4; i < d.size(); i += 1 + rng() % 5) d[i] = (rng() & 1) ? 0x00 : 0xFF;
        Bytes t = xf_apply(XF_X86, 0, d.data(), d.size());
        CHECK(xf_invert(XF_X86, 0, t.data(), t.size()) == d);
    }
    // The filter pays off on call-heavy code (level 1 may select it) and always round-trips through the codec.
    Bytes code = fake_code(60000, 4000);
    Bytes a = rt(code, kDefaultBlock, 0), b = rt(code, kDefaultBlock, 1);
    CHECK(b.size() < a.size());
}

static void test_cfg() {
    std::string t; for (int i = 0; i < 400; ++i) t += "pack my box with five dozen liquor jugs " + std::to_string(i % 17) + "\n";
    const Bytes d = str(t);
    size_t sizes[2] = {0, 0};
    for (int lite = 0; lite <= 1; ++lite)
        for (int mb : {16, 18, 22, 24}) {
            CmCfg cfg; cfg.lite = uint8_t(lite); cfg.membits = uint8_t(mb);
            Bytes e = cm_encode(d.data(), d.size(), 0, cfg);
            CHECK(cm_decode(e.data(), e.size(), d.size(), 0, cfg) == d);
            if (mb == 22) sizes[lite] = e.size();
        }
    CHECK(sizes[0] < sizes[1]);                       // full model beats lite on text
    for (int mb : {16, 20, 24}) {                     // container-level, both profiles, stored memory setting
        for (int lvl : {0, 1}) {
            Bytes blob = compress(d, kDefaultBlock, lvl, 1, mb);
            CHECK(decompress(blob, uint64_t(1) << 32, 1) == d);
        }
    }
    bool thrown = false;
    try { compress(d, kDefaultBlock, 1, 1, 15); } catch (const std::invalid_argument&) { thrown = true; }
    CHECK(thrown);
    thrown = false;
    try { compress(d, kDefaultBlock, 1, 1, 25); } catch (const std::invalid_argument&) { thrown = true; }
    CHECK(thrown);
    // a block whose stored memory setting exceeds the maximum must be refused before decoding
    Bytes blob = compress(d, kDefaultBlock, 1, 1);
    size_t pos = 5; auto skipv = [&]() { while (blob[pos] & 0x80) ++pos; ++pos; };
    skipv(); skipv();                                 // total, block size
    ++pos; skipv();                                   // method, raw length
    int xf = blob[pos++];
    int np = (xf >= 1 && xf <= 3) ? 1 : xf == 4 ? 5 : 0;
    for (int i = 0; i < np; ++i) skipv();
    skipv();                                          // stride
    Bytes bad = blob; bad[pos] = uint8_t(1 | (30 << 1));
    CHECK(throws(bad));
}

static void test_records() {
    // fixed-length records (period 37): detection must keep the archive far below raw and round-trip
    Bytes d; 
    for (int r = 0; r < 3000; ++r) {
        uint32_t id = uint32_t(r) * 7;
        for (int k = 0; k < 4; ++k) d.push_back(uint8_t(id >> (8 * k)));
        for (int k = 0; k < 33; ++k) d.push_back(uint8_t((k * 31 + (r % 5)) & 0xFF));
    }
    Bytes blob = rt(d, kDefaultBlock, 1);
    CHECK(blob.size() < d.size() / 8);
}

int main() {
    test_transforms();
    test_codec();
    test_images();
    test_malformed();
    test_x86();
    test_cfg();
    test_records();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
