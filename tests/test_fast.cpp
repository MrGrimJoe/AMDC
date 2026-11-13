// AMDC-F tests. Exit status 0 = all passed. Run under ASan/UBSan too (make sanitize).
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <memory>
#include <thread>
#include <vector>

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

static void append_lz4_len(Bytes& b, size_t n) {
    while (n >= 255) { b.push_back(255); n -= 255; }
    b.push_back(uint8_t(n));
}

// Long final matches are the case this regression test targets.
static void test_tight_match_copy() {
    for (size_t offset = 2; offset <= 40; ++offset) {
        for (size_t match_len : {size_t(127), size_t(128), size_t(129), size_t(255), size_t(256), size_t(257), size_t(1024), size_t(4096)}) {
            for (size_t tail_len : {size_t(5), size_t(15), size_t(32)}) {
                Bytes enc;
                const size_t lit_nibble = std::min<size_t>(15, offset);
                const size_t ml_code = match_len - 4;
                enc.push_back(uint8_t((lit_nibble << 4) | std::min<size_t>(15, ml_code)));
                if (offset >= 15) append_lz4_len(enc, offset - 15);
                for (size_t i = 0; i < offset; ++i) enc.push_back(uint8_t('A' + i));
                enc.push_back(uint8_t(offset));
                enc.push_back(uint8_t(offset >> 8));
                if (ml_code >= 15) append_lz4_len(enc, ml_code - 15);
                if (tail_len < 15) enc.push_back(uint8_t(tail_len << 4));
                else {
                    enc.push_back(0xF0);
                    append_lz4_len(enc, tail_len - 15);
                }
                for (size_t i = 0; i < tail_len; ++i) enc.push_back(uint8_t('x' + (i % 8)));

                Bytes expected;
                for (size_t i = 0; i < offset; ++i) expected.push_back(uint8_t('A' + i));
                for (size_t i = 0; i < match_len; ++i)
                    expected.push_back(expected[expected.size() - offset]);
                for (size_t i = 0; i < tail_len; ++i) expected.push_back(uint8_t('x' + (i % 8)));

                Bytes out(expected.size());
                CHECK(block_decompress(enc.data(), enc.size(), out.data(), out.size()));
                CHECK(out == expected);
            }
        }
    }
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

// ---- differential testing against a deliberately naive reference decoder --------------------------------
static bool ref_decompress(const uint8_t* ip, size_t inlen, Bytes& out, size_t outlen) {
    const uint8_t* const iend = ip + inlen;
    out.clear();
    for (;;) {
        if (ip >= iend) return false;
        const unsigned token = *ip++;
        size_t lit = token >> 4;
        if (lit == 15) { unsigned s; do { if (ip >= iend) return false; s = *ip++; lit += s; if (lit > outlen) return false; } while (s == 255); }
        if (lit > size_t(iend - ip) || out.size() + lit > outlen) return false;
        out.insert(out.end(), ip, ip + lit);
        ip += lit;
        if (ip == iend) return out.size() == outlen;
        if (iend - ip < 2) return false;
        const size_t offset = size_t(ip[0]) | (size_t(ip[1]) << 8);
        ip += 2;
        if (offset == 0 || offset > out.size()) return false;
        size_t mlen = token & 15;
        if (mlen == 15) { unsigned s; do { if (ip >= iend) return false; s = *ip++; mlen += s; if (mlen > outlen) return false; } while (s == 255); }
        mlen += 4;
        if (out.size() + mlen > outlen) return false;
        for (size_t i = 0; i < mlen; ++i) out.push_back(out[out.size() - offset]);
    }
}

// Exact-size heap buffer so ASan flags any write past the end.
static bool decode_exact(const Bytes& enc, size_t outlen, Bytes& got) {
    std::unique_ptr<uint8_t[]> buf(new uint8_t[outlen ? outlen : 1]);
    const bool ok = block_decompress(enc.data(), enc.size(), buf.get(), outlen);
    if (ok) got.assign(buf.get(), buf.get() + outlen);
    return ok;
}

static void put_len(Bytes& b, size_t n) { while (n >= 255) { b.push_back(255); n -= 255; } b.push_back(uint8_t(n)); }

// Random but valid sequences: literal runs, matches of every offset class, ending in a literal-only token.
static void test_differential() {
    for (int iter = 0; iter < 3000; ++iter) {
        Bytes enc; size_t produced = 0;
        const int seqs = 1 + int(rng() % 6);
        for (int s = 0; s < seqs; ++s) {
            size_t lit = (produced == 0) ? 1 + rng() % 40 : rng() % 40;
            size_t offset = 1 + rng() % std::min<size_t>(produced + lit, (rng() % 4 == 0) ? 70000 : 48);
            offset = std::min<size_t>(offset, std::min<size_t>(produced + lit, 65535));
            const size_t mlen = 4 + ((rng() % 3 == 0) ? rng() % 6000 : rng() % 40);
            const size_t lc = std::min<size_t>(15, lit), mc = std::min<size_t>(15, mlen - 4);
            enc.push_back(uint8_t((lc << 4) | mc));
            if (lit >= 15) put_len(enc, lit - 15);
            for (size_t i = 0; i < lit; ++i) enc.push_back(uint8_t(rng()));
            enc.push_back(uint8_t(offset)); enc.push_back(uint8_t(offset >> 8));
            if (mlen - 4 >= 15) put_len(enc, mlen - 4 - 15);
            produced += lit + mlen;
        }
        const size_t tail = (rng() % 3 == 0) ? 5 + rng() % 8 : 5 + rng() % 200;
        enc.push_back(tail < 15 ? uint8_t(tail << 4) : uint8_t(0xF0));
        if (tail >= 15) put_len(enc, tail - 15);
        for (size_t i = 0; i < tail; ++i) enc.push_back(uint8_t(rng()));
        const size_t total = produced + tail;
        Bytes ref, got;
        const bool rok = ref_decompress(enc.data(), enc.size(), ref, total);
        const bool dok = decode_exact(enc, total, got);
        CHECK(rok && dok && ref == got);
    }
}

// Mutated blocks: the fast decoder must agree with the reference on accept/reject and on the bytes produced.
static void test_differential_mutation() {
    for (int base = 0; base < 6; ++base) {
        Bytes d = (base % 2) ? periodic(30000, 1 + base * 7) : texty(30000);
        Bytes c(block_bound(d.size()));
        c.resize(block_compress(d.data(), d.size(), c.data(), 1 + base % 2));
        for (int it = 0; it < 20000; ++it) {
            Bytes m = c;
            int k = 1 + int(rng() % 3);
            while (k--) m[rng() % m.size()] = uint8_t(rng());
            const size_t outlen = (rng() % 5 == 0) ? d.size() + rng() % 5 - 2 : d.size();
            Bytes ref, got;
            const bool rok = ref_decompress(m.data(), m.size(), ref, outlen);
            const bool dok = decode_exact(m, outlen, got);
            CHECK(rok == dok);
            if (rok && dok) CHECK(ref == got);
        }
    }
}

// Tight-output short-period matches (offset 2..15): the final-match fallback. Every offset x every match length
// 4..300 (shorter and longer than one period) x several literal tails, decoded into an exact-size heap buffer.
static void test_short_period_tight() {
    for (size_t offset = 2; offset <= 15; ++offset) {
        for (size_t mlen = 4; mlen <= 300; ++mlen) {
            for (size_t tail : {size_t(5), size_t(6), size_t(11), size_t(15), size_t(16)}) {
                Bytes enc, expected;
                const size_t mc = mlen - 4;
                enc.push_back(uint8_t((std::min<size_t>(15, offset) << 4) | std::min<size_t>(15, mc)));
                if (offset >= 15) put_len(enc, offset - 15);
                for (size_t i = 0; i < offset; ++i) { enc.push_back(uint8_t(rng())); expected.push_back(enc.back()); }
                enc.push_back(uint8_t(offset)); enc.push_back(uint8_t(offset >> 8));
                if (mc >= 15) put_len(enc, mc - 15);
                for (size_t i = 0; i < mlen; ++i) expected.push_back(expected[expected.size() - offset]);
                enc.push_back(tail < 15 ? uint8_t(tail << 4) : uint8_t(0xF0));
                if (tail >= 15) put_len(enc, tail - 15);
                for (size_t i = 0; i < tail; ++i) { enc.push_back(uint8_t(rng())); expected.push_back(enc.back()); }
                Bytes got;
                CHECK(decode_exact(enc, expected.size(), got));
                CHECK(got == expected);
                // too-small and too-large declared sizes must be rejected, never over-written
                Bytes sink;
                CHECK(!decode_exact(enc, expected.size() - 1, sink));
                CHECK(!decode_exact(enc, expected.size() + 1, sink));
                // truncated stream is rejected
                Bytes cut(enc.begin(), enc.end() - 1);
                CHECK(!decode_exact(cut, expected.size(), sink));
            }
        }
    }
}

// Mutations of blocks whose LAST match is a short-offset tight match: must agree with the naive reference.
static void test_short_period_mutation() {
    for (int it = 0; it < 30000; ++it) {
        const size_t offset = 2 + rng() % 14, mlen = 4 + rng() % 140, tail = 5 + rng() % 4;
        Bytes enc;
        const size_t mc = mlen - 4;
        enc.push_back(uint8_t((std::min<size_t>(15, offset) << 4) | std::min<size_t>(15, mc)));
        if (offset >= 15) put_len(enc, offset - 15);
        for (size_t i = 0; i < offset; ++i) enc.push_back(uint8_t(rng()));
        enc.push_back(uint8_t(offset)); enc.push_back(0);
        if (mc >= 15) put_len(enc, mc - 15);
        enc.push_back(uint8_t(tail << 4));
        for (size_t i = 0; i < tail; ++i) enc.push_back(uint8_t(rng()));
        const size_t total = offset + mlen + tail;
        Bytes m = enc;
        for (int k = 1 + int(rng() % 2); k--;) m[rng() % m.size()] = uint8_t(rng());
        const size_t outlen = (rng() % 4 == 0) ? total + rng() % 5 - 2 : total;
        Bytes ref, got;
        const bool rok = ref_decompress(m.data(), m.size(), ref, outlen);
        const bool dok = decode_exact(m, outlen, got);
        CHECK(rok == dok);
        if (rok && dok) CHECK(ref == got);
    }
}

// Concurrent put/get/erase on a shared cache: every successful get must return a complete, valid version.
static void test_cache_concurrency() {
    const size_t P = 4096;
    CompressedPageCache cache(P);
    std::vector<Bytes> versions;
    for (int v = 0; v < 4; ++v) versions.push_back(v == 3 ? rnd(P) : (v == 2 ? Bytes(P, 0) : texty(P)));
    std::atomic<int> bad{0};
    auto worker = [&](unsigned seed) {
        std::mt19937 r(seed); Bytes got(P);
        for (int i = 0; i < 20000; ++i) {
            const uint64_t id = r() % 32;
            switch (r() % 4) {
                case 0: case 1: cache.put(id, versions[r() % versions.size()].data()); break;
                case 2: cache.erase(id); break;
                default:
                    if (cache.get(id, got.data())) {
                        bool match = false;
                        for (const Bytes& v : versions) match = match || (got == v);
                        if (!match) ++bad;
                    }
            }
        }
    };
    std::vector<std::thread> th;
    for (unsigned i = 0; i < 6; ++i) th.emplace_back(worker, 100 + i);
    for (auto& t : th) t.join();
    CHECK(bad == 0);
    for (uint64_t id = 0; id < 32; ++id) cache.erase(id);
    CHECK(cache.compressed_bytes() == 0);                                   // accounting survives the races
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
    test_tight_match_copy();
    test_block_malformed();
    test_differential();
    test_differential_mutation();
    test_short_period_tight();
    test_short_period_mutation();
    test_container();
    test_pages();
    test_cache_concurrency();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
