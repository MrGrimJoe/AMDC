// Tests for the BWT tier (AMDC-B): transform vs brute force, codec round trips, determinism, malformed input.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>

#include "../src/bwt.hpp"
#include "../src/bwtcodec.hpp"

using namespace amdc1;
using bwtc::Bytes;

static int g_checks = 0, g_fail = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static std::mt19937_64 rng(12345);

static Bytes rand_bytes(size_t n, int alpha) {
    Bytes b(n);
    for (auto& c : b) c = uint8_t(rng() % alpha);
    return b;
}
static Bytes text(size_t n) {
    static const char* w[] = {"the ", "quick ", "brown ", "fox ", "jumps ", "over ", "lazy ", "dog\n", "pack ", "my ", "box "};
    Bytes b;
    while (b.size() < n) { const char* s = w[rng() % 11]; b.insert(b.end(), s, s + std::strlen(s)); }
    b.resize(n);
    return b;
}
static bool throws(const Bytes& blob) {
    try { bwtc::decompress(blob); } catch (const std::exception&) { return true; }
    return false;
}

static void test_transform() {
    for (int round = 0; round < 1500; ++round) {
        const size_t n = 1 + rng() % 120;
        int alpha = 1 + int(rng() % 6);
        if (rng() % 5 == 0) alpha = 256;
        Bytes d = rand_bytes(n, alpha);
        if (rng() % 4 == 0) for (size_t i = 1; i < n; ++i) if (rng() % 3) d[i] = d[i - 1];
        Bytes L(n), o(n);
        bwt::Header h;
        bwt::forward(d.data(), n, L.data(), h);
        std::vector<int> idx(n + 1);
        for (size_t i = 0; i <= n; ++i) idx[i] = int(i);
        std::sort(idx.begin(), idx.end(), [&](int a, int b) {       // suffixes of d + end marker (smallest symbol)
            for (;; ++a, ++b) {
                if (a == int(n)) return true;
                if (b == int(n)) return false;
                if (d[a] != d[b]) return d[a] < d[b];
            }
        });
        Bytes ref;
        for (size_t r = 0; r <= n; ++r) if (idx[r] != 0) ref.push_back(d[idx[r] - 1]);
        CHECK(ref == L);
        CHECK(bwt::inverse(L.data(), n, h, o.data()) && o == d);
    }
    Bytes L(8), o(8);
    bwt::Header h;
    h.primary = 100;
    CHECK(!bwt::inverse(L.data(), 8, h, o.data()));                  // inconsistent header is refused
    h.primary = 0; h.starts[1] = 99;
    CHECK(!bwt::inverse(L.data(), 8, h, o.data()));
}

static Bytes rt(const Bytes& d, size_t block, unsigned threads = 1) {
    Bytes blob = bwtc::compress(d, block, threads);
    CHECK(bwtc::decompress(blob, uint64_t(1) << 32, threads) == d);
    return blob;
}

static void test_codec() {
    for (size_t n : {0, 1, 2, 15, 16, 17, 31, 100, 1000}) { rt(rand_bytes(n, 256), 64); rt(text(n), 64); }
    rt(Bytes(100000, 0), 4096);
    rt(Bytes(70001, 'a'), 70001);
    rt(rand_bytes(50000, 256), 16384);
    rt(rand_bytes(50000, 2), 16384);
    Bytes t = text(300000);
    rt(t, 100000);                                                    // exact multiple of the block size
    rt(Bytes(t.begin(), t.begin() + 299999), 100000);                 // short last block
    Bytes a = rt(t, 40000, 1), b = rt(t, 40000, 4);
    CHECK(a == b);                                                    // thread count never changes the archive
    CHECK(a.size() < t.size() / 3);                                   // actually compresses text
    Bytes r = rand_bytes(40000, 256);
    CHECK(rt(r, 16384).size() < r.size() + 100);                      // incompressible data is stored, not expanded
    // binary with structure
    Bytes s;
    for (int i = 0; i < 20000; ++i) { s.push_back(uint8_t(i)); s.push_back(uint8_t(i >> 8)); s.push_back(7); s.push_back(0); }
    rt(s, 30000);
}

static void test_rank_coder() {
    for (int round = 0; round < 40; ++round) {
        Bytes d = rng() % 2 ? rand_bytes(1 + rng() % 5000, 1 + int(rng() % 40)) : text(1 + rng() % 5000);
        Bytes e = bwtc::rank_encode(d.data(), d.size());
        CHECK(bwtc::rank_decode(e.data(), e.size(), d.size()) == d);
    }
}

static void test_malformed() {
    Bytes t = text(30000);
    Bytes blob = bwtc::compress(t, 8192, 1);
    CHECK(bwtc::decompress(blob) == t);
    for (size_t cut = 0; cut < blob.size(); cut += 1 + blob.size() / 300) {   // every truncation must be refused
        Bytes tr(blob.begin(), blob.begin() + long(cut));
        CHECK(throws(tr));
    }
    Bytes bad = blob; bad[0] = 'X'; CHECK(throws(bad));
    bad = blob; bad[4] = 9; CHECK(throws(bad));
    bad = blob; bad.push_back(0); CHECK(throws(bad));                         // trailing data
    bool limited = false;
    try { bwtc::decompress(blob, 100); } catch (const bwtc::FormatError&) { limited = true; }
    CHECK(limited);
    // header claiming an enormous output from a tiny archive
    Bytes bomb = {'A', 'M', 'D', 'B', 1, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x10};
    CHECK(throws(bomb));
    // random single-byte mutations: must either throw or return the original (the checksum guarantees it), never crash
    int accepted_wrong = 0;
    for (int round = 0; round < 3000; ++round) {
        Bytes m = blob;
        const size_t k = rng() % m.size();
        m[k] = uint8_t(m[k] ^ (1 + rng() % 255));
        try {
            Bytes o = bwtc::decompress(m, uint64_t(1) << 28);
            if (o != t) ++accepted_wrong;
        } catch (const std::exception&) {}
    }
    CHECK(accepted_wrong == 0);
    // multi-byte garbage bodies
    for (int round = 0; round < 500; ++round) {
        Bytes m(blob.begin(), blob.begin() + 12);
        for (int i = 0; i < 100; ++i) m.push_back(uint8_t(rng()));
        try { bwtc::decompress(m, uint64_t(1) << 28); } catch (const std::exception&) {}
    }
}

int main() {
    test_transform();
    test_rank_coder();
    test_codec();
    test_malformed();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail != 0;
}
