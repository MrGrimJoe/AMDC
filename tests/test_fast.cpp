// AMDC-F tests. Exit status 0 = all passed. Run under ASan/UBSan too (make sanitize).
#include <cstdio>
#include <cstring>
#include <random>
#include <string>

#include "../src/fast.hpp"
#include "../src/pagecache.hpp"

using namespace amdc1::fast;

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::mt19937 rng(42);
static Bytes rnd(size_t n, int mod = 256) { Bytes b(n); for (auto& c : b) c = uint8_t(rng() % mod); return b; }

static bool block_rt(const Bytes& d, int level) {
    Bytes c(block_bound(d.size()));
    c.resize(block_compress(d.data(), d.size(), c.data(), level));
    Bytes o(d.size());
    return block_decompress(c.data(), c.size(), o.data(), d.size()) && o == d;
}

static Bytes periodic(size_t n, size_t period) {
    Bytes b(n); Bytes pat = rnd(period);
    for (size_t i = 0; i < n; ++i) b[i] = pat[i % period];
    return b;
}

static Bytes texty(size_t n) {
    static const char* w[] = {"the ", "quick ", "brown ", "fox ", "jumps ", "over ", "lazy ", "dog. ", "alpha ", "beta "};
    std::string s;
    while (s.size() < n) s += w[rng() % 10];
    s.resize(n);
    return Bytes(s.begin(), s.end());
}

static void test_blocks() {
    for (int lvl = 1; lvl <= 2; ++lvl) {
        for (size_t n = 0; n <= 300; ++n) {                       // every tiny length, incl. format-limit edges
            CHECK(block_rt(rnd(n, 4), lvl));
            CHECK(block_rt(Bytes(n, 7), lvl));
        }
        for (size_t p = 1; p <= 40; ++p) CHECK(block_rt(periodic(5000 + p, p), lvl));   // every short match offset
        for (size_t n : {1000u, 65535u, 65536u, 65537u, 200000u}) {
            CHECK(block_rt(texty(n), lvl));
            CHECK(block_rt(rnd(n), lvl));
            CHECK(block_rt(rnd(n, 3), lvl));
        }
        Bytes mixed = texty(30000), z(70000, 0), r = rnd(20000);       // long matches (length extension bytes), far matches
        mixed.insert(mixed.end(), z.begin(), z.end());
        mixed.insert(mixed.end(), r.begin(), r.end());
        Bytes again = texty(30000);
        mixed.insert(mixed.end(), again.begin(), again.end());
        CHECK(block_rt(mixed, lvl));
    }
    Bytes t = texty(100000), c1(block_bound(t.size())), c2(block_bound(t.size()));
    c1.resize(block_compress(t.data(), t.size(), c1.data(), 1));
    c2.resize(block_compress(t.data(), t.size(), c2.data(), 2));
    CHECK(c1.size() < t.size() * 6 / 10);
    CHECK(c2.size() < t.size() * 3 / 10);                                   // lazy hash-chain search finds far more
    CHECK(c2.size() <= c1.size());
}

static void test_block_malformed() {
    Bytes d = texty(20000), c(block_bound(d.size()));
    c.resize(block_compress(d.data(), d.size(), c.data(), 2));
    Bytes out(d.size());
    CHECK(block_decompress(c.data(), c.size(), out.data(), d.size()));
    CHECK(!block_decompress(c.data(), c.size(), out.data(), d.size() - 1));        // wrong declared size
    CHECK(!block_decompress(c.data(), c.size(), out.data(), d.size() + 1));
    CHECK(!block_decompress(c.data(), c.size() - 1, out.data(), d.size()));        // truncated
    CHECK(!block_decompress(c.data(), 0, out.data(), d.size()));
    // 200k random mutations: must never crash, over-read or over-write (ASan build verifies); result is irrelevant
    std::vector<uint8_t> buf(d.size() + 64);
    for (int it = 0; it < 200000; ++it) {
        Bytes m = c;
        int k = 1 + int(rng() % 4);
        while (k--) m[rng() % m.size()] = uint8_t(rng());
        if (rng() % 8 == 0) m.resize(rng() % m.size());
        (void)block_decompress(m.data(), m.size(), buf.data(), d.size());
    }
    // offset pointing before the start of output
    uint8_t bad[] = {0x10, 'a', 0x05, 0x00, 0x00};
    uint8_t o2[16];
    CHECK(!block_decompress(bad, sizeof bad, o2, 9));
}

static void test_container() {
    Bytes d = texty(300000), z(100000, 0), r = rnd(50000);
    d.insert(d.end(), z.begin(), z.end());
    d.insert(d.end(), r.begin(), r.end());
    for (int lvl = 1; lvl <= 2; ++lvl) {
        for (size_t bs : {size_t(4096), size_t(65536), kDefaultBlock}) {
            Bytes c = compress(d, bs, lvl, 1, true);
            CHECK(decompress(c, uint64_t(1) << 32, 1) == d);
            CHECK(compress(d, bs, lvl, 4, true) == c);                        // thread-count invariant
            CHECK(decompress(c, uint64_t(1) << 32, 4) == d);
        }
    }
    for (const Bytes& e : {Bytes{}, Bytes{1}, Bytes(4096, 9), rnd(5000)}) {
        CHECK(decompress(compress(e, 4096, 1, 1, true)) == e);
        CHECK(decompress(compress(e, 4096, 1, 1, false)) == e);
    }
    Bytes c = compress(d, 65536, 1, 1, true), o(d.size());
    CHECK(decompressed_size(c.data(), c.size()) == d.size());
    CHECK(decompress_into(c.data(), c.size(), o.data(), o.size()) == d.size() && o == d);
    bool threw = false;
    try { decompress_into(c.data(), c.size(), o.data(), o.size() - 1); } catch (const FastError&) { threw = true; }
    CHECK(threw);
    // corruption: every flip either throws or (never) yields wrong data
    int bad_silent = 0;
    for (size_t i = 0; i < c.size(); i += std::max<size_t>(1, c.size() / 400)) {
        Bytes m = c; m[i] ^= 0x40;
        try { if (decompress(m, uint64_t(1) << 32, 1) != d) ++bad_silent; } catch (const FastError&) {}
    }
    CHECK(bad_silent == 0);
    for (size_t cut : {size_t(0), size_t(5), size_t(9), c.size() - 1}) {
        bool t = false;
        try { decompress(Bytes(c.begin(), c.begin() + long(cut))); } catch (const FastError&) { t = true; }
        CHECK(t);
    }
    Bytes g = c; g.push_back(0);
    bool t2 = false; try { decompress(g); } catch (const FastError&) { t2 = true; }
    CHECK(t2);
    bool t3 = false; try { decompress(c, 100); } catch (const FastError&) { t3 = true; }
    CHECK(t3);
}

static void test_pages() {
    const size_t P = 4096;
    Bytes enc(page_encode_bound(P)), out(P);
    Bytes zero(P, 0), fill(P, 0xAB), text = texty(P), noise = rnd(P);
    CHECK(page_compress(zero.data(), P, enc.data()) == 1 && enc[0] == PAGE_ZERO);
    CHECK(page_decompress(enc.data(), 1, out.data(), P) && out == zero);
    CHECK(page_compress(fill.data(), P, enc.data()) == 2 && enc[0] == PAGE_FILL);
    CHECK(page_decompress(enc.data(), 2, out.data(), P) && out == fill);
    size_t n = page_compress(text.data(), P, enc.data(), 1);
    CHECK(n > 0 && n < P * 3 / 4 && enc[0] == PAGE_LZ);
    CHECK(page_decompress(enc.data(), n, out.data(), P) && out == text);
    CHECK(page_compress(noise.data(), P, enc.data()) == 0);                 // incompressible: caller keeps raw
    CHECK(!page_decompress(enc.data(), 0, out.data(), P));
    uint8_t junk[3] = {9, 0, 0};
    CHECK(!page_decompress(junk, 3, out.data(), P));

    CompressedPageCache cache(P);
    Bytes got(P);
    CHECK(!cache.get(1, got.data()));
    cache.put(1, text.data()); cache.put(2, noise.data()); cache.put(3, zero.data()); cache.put(4, fill.data());
    for (auto& kv : std::vector<std::pair<uint64_t, const Bytes*>>{{1, &text}, {2, &noise}, {3, &zero}, {4, &fill}}) {
        CHECK(cache.get(kv.first, got.data()) && got == *kv.second);
    }
    size_t before = cache.compressed_bytes();
    cache.put(2, zero.data());                                              // overwrite shrinks the accounting
    CHECK(cache.compressed_bytes() < before);
    CHECK(cache.get(2, got.data()) && got == zero);
    CHECK(cache.erase(2) && !cache.get(2, got.data()) && !cache.erase(2));
}

int main() {
    test_blocks();
    test_block_malformed();
    test_container();
    test_pages();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
