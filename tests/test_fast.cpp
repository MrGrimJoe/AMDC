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


// ---- page formats v2 / v3 ---------------------------------------------------------------------------------------
// deliberately naive page2 decoder: one byte at a time, every bound checked, written from the format description
static bool ref_page2(const uint8_t* ip, size_t inlen, Bytes& out, size_t outlen) {
    out.clear();
    const uint8_t* e = ip + inlen;
    size_t last = 0;
    for (;;) {
        if (ip >= e) return false;
        unsigned t = *ip++;
        size_t lit = t >> 4;
        if (lit == 15) { unsigned b; do { if (ip >= e) return false; b = *ip++; lit += b; if (lit > outlen) return false; } while (b == 255); }
        if (lit > size_t(e - ip) || out.size() + lit > outlen) return false;
        for (size_t i = 0; i < lit; ++i) out.push_back(*ip++);
        if (ip == e) return out.size() == outlen;
        size_t off;
        unsigned o = *ip++;
        if (o == 0) off = last;
        else if (o < 0xF0) off = o;
        else { if (ip >= e) return false; off = ((size_t(o & 15) << 8) | *ip++) + 240; }
        last = off;
        if (off == 0 || off > out.size()) return false;
        size_t ml = t & 15;
        if (ml == 15) { unsigned b; do { if (ip >= e) return false; b = *ip++; ml += b; if (ml > outlen) return false; } while (b == 255); }
        ml += 4;
        if (out.size() + ml > outlen) return false;
        for (size_t i = 0; i < ml; ++i) out.push_back(out[out.size() - off]);
    }
}

static Bytes structured(size_t n, int stride) {      // like an array of small records: counters, pointers, flags
    Bytes b(n);
    for (size_t i = 0; i < n; ++i) {
        size_t rec = i / stride, f = i % stride;
        b[i] = f == 0 ? uint8_t(rec) : f < 3 ? uint8_t(rec >> 8) : f == 3 ? 0x7f : f < 6 ? 0 : uint8_t(rng() % 5);
    }
    return b;
}

static Bytes read_file(const char* path) {
    Bytes b;
    if (FILE* f = std::fopen(path, "rb")) {
        uint8_t buf[65536];
        size_t r;
        while ((r = std::fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + r);
        std::fclose(f);
    }
    return b;
}

static std::vector<Bytes> page_samples(size_t n) {
    std::vector<Bytes> v;
    if (n == 4096) {                                     // real text and real machine code, so the built-in tables get used
        static const Bytes files[2] = {read_file("src/fast.cpp"), read_file("build/test_fast")};
        for (const Bytes& f : files)
            for (size_t k = 0; k < 6 && (k + 1) * 4096 * 3 <= f.size(); ++k)
                v.emplace_back(f.begin() + long(k * 4096 * 3), f.begin() + long(k * 4096 * 3 + 4096));
    }
    v.push_back(texty(n)); v.push_back(rnd(n, 3)); v.push_back(rnd(n, 16)); v.push_back(rnd(n, 64)); v.push_back(rnd(n));
    v.push_back(periodic(n, 7)); v.push_back(periodic(n, 100)); v.push_back(periodic(n, 700));
    v.push_back(structured(n, 8)); v.push_back(structured(n, 24)); v.push_back(structured(n, 64));
    for (int kind = 0; kind < 7; ++kind) {              // pointer / counter / index / float-like word arrays (word-transform fodder)
        Bytes w(n);
        uint64_t base = 0x00007f3a12000000ull + (rng() & 0xffff0000ull), ctr = rng() & 0xffff;
        for (size_t i = 0; i + 8 <= n; i += 8) {
            uint64_t x = 0;
            if (kind >= 4) {                                                       // 32-bit element arrays
                for (int h = 0; h < 2; ++h) {
                    uint32_t y = kind == 4 ? uint32_t(ctr += rng() % 5)            // slowly growing 32-bit counters
                               : kind == 5 ? 0x08040000u + uint32_t(rng() % 60000)  // 32-bit addresses, shared high bytes
                                           : 0x3f800000u + uint32_t(rng() % 4096) * 2;   // floats near 1.0
                    std::memcpy(&w[i + 4 * h], &y, 4);
                }
                continue;
            }
            switch (kind) {
                case 0: x = base + (rng() % 4096) * 16; break;                     // heap pointers
                case 1: x = ctr += 1 + rng() % 3; break;                           // increasing counters
                case 2: x = uint64_t(rng() % 1000) | (uint64_t(rng() % 7) << 32); break;   // small ints, 32-bit halves
                default: x = 0x4000000000000000ull | (uint64_t(rng() & 0xfffff) << 20); break;   // doubles of similar magnitude
            }
            std::memcpy(&w[i], &x, 8);
        }
        v.push_back(w);
    }
    Bytes mix = texty(n);
    for (size_t i = n / 3; i < n / 2; ++i) mix[i] = uint8_t(rng());
    v.push_back(mix);
    return v;
}

static void test_page3() {
    size_t huf_used = 0, lz2_used = 0, huf4_used = 0, litc_used = 0, split_used[4] = {0, 0, 0, 0}, preset_used = 0, xf_used[4] = {0, 0, 0, 0};
    for (size_t n : {size_t(1), size_t(15), size_t(16), size_t(17), size_t(100), size_t(511), size_t(1000), size_t(4095), size_t(4096), size_t(4336)}) {
        for (const Bytes& pg : page_samples(n)) {
            Bytes enc(page3_bound(n)), out(n);
            for (int lvl = 1; lvl <= 6; ++lvl) {
                size_t c = page3_compress(pg.data(), n, enc.data(), lvl);
                if (!c) continue;
                CHECK(c < n);
                if (enc[0] == PAGE_XFORM) { CHECK(enc[1] >= 1 && enc[1] <= 3); ++xf_used[enc[1]]; }
                if (enc[0] == PAGE_HUF) { ++huf_used; litc_used += (enc[1] & 1) != 0; preset_used += (enc[1] & 4) != 0; }
                if (enc[0] == PAGE_HUF4) {
                    ++huf4_used;
                    for (int b = 0; b < 4; ++b) split_used[b] += ((enc[1] >> (2 * b)) & 3) != 0;
                    for (int b = 0; b < 4; ++b) preset_used += ((enc[1] >> (2 * b)) & 3) == 2;
                }
                if (enc[0] == PAGE_LZ2) ++lz2_used;
                std::fill(out.begin(), out.end(), 0x55);
                CHECK(page3_decompress(enc.data(), c, out.data(), n) && out == pg);
                if (enc[0] == PAGE_HUF4) {                       // the direct three-stream decoder against the re-interleaving one
                    Bytes o2(n, 0x33);
                    CHECK(page4_decompress_reference(enc.data(), c, o2.data(), n) && o2 == pg);
                }
                Bytes sink(n + 1);
                CHECK(!page3_decompress(enc.data(), c, sink.data(), n + 1) || enc[0] == PAGE_FILL || enc[0] == PAGE_ZERO);
                if (n > 1) CHECK(!page3_decompress(enc.data(), c, sink.data(), n - 1) || enc[0] == PAGE_FILL || enc[0] == PAGE_ZERO);
                CHECK(!page3_decompress(enc.data(), c - 1, out.data(), n) || enc[0] == PAGE_FILL);
            }
            // page2 against the naive decoder, on the valid stream
            if (n >= 16) {
                Bytes e2(block_bound(n)), ref;
                for (int lvl = 1; lvl <= 2; ++lvl) {
                    size_t c2 = page2_compress(pg.data(), n, e2.data(), lvl);
                    Bytes o2(n);
                    CHECK(page2_decompress(e2.data(), c2, o2.data(), n) && o2 == pg);
                    CHECK(ref_page2(e2.data(), c2, ref, n) && ref == pg);
                }
            }
        }
    }
    CHECK(huf_used > 20 && lz2_used > 20 && huf4_used > 10 && litc_used > 20);   // every blob variant is really exercised
    CHECK(split_used[0] > 5 && split_used[1] > 5 && split_used[2] > 5);          // literal, token and offset blocks are coded
    CHECK(preset_used > 5);                                                      // built-in tables are used and round-trip
    std::printf("page3 kinds exercised: huf %zu (lit-coded %zu), huf4 %zu (coded blocks lit/tok/off/ext %zu/%zu/%zu/%zu), preset blocks %zu, lz2 %zu\n",
                huf_used, litc_used, huf4_used, split_used[0], split_used[1], split_used[2], split_used[3], preset_used, lz2_used);
    CHECK(xf_used[1] > 0 && xf_used[2] > 0 && xf_used[3] > 0);        // all three transforms are really exercised
    std::printf("page3 transforms: shuf8 %zu, shuf4 %zu, delta4+shuf4 %zu\n", xf_used[1], xf_used[2], xf_used[3]);
}

static void test_page3_mutation() {
    // random corruption of valid blobs: never crash / over-read / over-write (ASan build checks);
    // page2 streams must additionally be accepted or rejected exactly like the naive decoder
    size_t decoded_ok = 0, total = 0;
    for (size_t n : {size_t(300), size_t(2048), size_t(4096)}) {
        for (const Bytes& pg : page_samples(n)) {
            for (int lvl : {1, 3, 4, 5, 6}) {
                Bytes enc(page3_bound(n));
                size_t c = page3_compress(pg.data(), n, enc.data(), lvl);
                if (c < 4) continue;
                for (int it = 0; it < 600; ++it) {
                    Bytes m(enc.begin(), enc.begin() + long(c));
                    int k = 1 + int(rng() % 3);
                    for (int j = 0; j < k; ++j) {
                        size_t pos = rng() % m.size();
                        switch (rng() % 3) { case 0: m[pos] ^= uint8_t(1u << (rng() % 8)); break; case 1: m[pos] = uint8_t(rng()); break; default: m[pos] = uint8_t(m[pos] + 1); }
                    }
                    if (rng() % 8 == 0) m.resize(1 + rng() % m.size());
                    Bytes out(n + 64, 0xCC);            // guard bytes after the page must stay untouched
                    const bool ok = page3_decompress(m.data(), m.size(), out.data(), n);
                    ++total; decoded_ok += ok;
                    for (size_t i = n; i < out.size(); ++i) if (out[i] != 0xCC) { CHECK(false); break; }
                    if (m[0] == PAGE_HUF4) {                 // must be accepted or rejected exactly like the reference decoder
                        Bytes o2(n + 64, 0xCC);
                        const bool rok = page4_decompress_reference(m.data(), m.size(), o2.data(), n);
                        CHECK(rok == ok);
                        if (rok && ok) CHECK(Bytes(o2.begin(), o2.begin() + long(n)) == Bytes(out.begin(), out.begin() + long(n)));
                        for (size_t i = n; i < o2.size(); ++i) if (o2[i] != 0xCC) { CHECK(false); break; }
                    }
                    if (m[0] == PAGE_LZ2 && n >= 16) {
                        Bytes ref;
                        const bool rok = ref_page2(m.data() + 1, m.size() - 1, ref, n);
                        CHECK(rok == ok);
                        if (rok && ok) CHECK(ref == Bytes(out.begin(), out.begin() + long(n)));
                    }
                }
            }
        }
    }
    std::printf("page3 mutation: %zu blobs, %zu still decoded\n", total, decoded_ok);
}

static void test_pool() {
    // a pool must give exactly the per-page result of page_compress for any thread count, round-trip, and count failures
    const size_t P = 4096;
    for (size_t count : {size_t(0), size_t(1), size_t(31), size_t(32), size_t(33), size_t(257), size_t(1000)}) {
        Bytes mem(count * P);
        std::vector<Bytes> shapes = page_samples(P);
        for (size_t i = 0; i < count; ++i) std::memcpy(&mem[i * P], shapes[(i * 7 + i / 3) % shapes.size()].data(), P);
        for (int level : {1, 2, 3, 5, 8}) {
            const size_t stride = page_encode_bound(P);
            std::vector<uint8_t> ref(count * stride + 1);
            std::vector<size_t> ref_len(count);
            for (size_t i = 0; i < count; ++i) ref_len[i] = page_compress(&mem[i * P], P, &ref[i * stride], level);
            for (unsigned th : {1u, 2u, 3u, 8u}) {
                PagePool pool(th);
                CHECK(pool.threads() >= 1);
                std::vector<uint8_t> enc(count * stride + 1);
                std::vector<size_t> len(count, 12345);
                pool.compress(mem.data(), P, count, enc.data(), stride, len.data(), level);
                std::vector<PageRef> refs(count);
                bool same = true;
                for (size_t i = 0; i < count; ++i) {
                    same &= len[i] == ref_len[i] && std::memcmp(&enc[i * stride], &ref[i * stride], len[i]) == 0;
                    refs[i] = PageRef{&enc[i * stride], len[i]};
                }
                CHECK(same);
                // pages that did not compress are stored raw by the caller; decode only the compressed ones
                std::vector<PageRef> good;
                std::vector<size_t> idx;
                for (size_t i = 0; i < count; ++i) if (len[i]) { good.push_back(refs[i]); idx.push_back(i); }
                Bytes out(good.size() * P + 1, 0x77);
                CHECK(pool.decompress(good.data(), good.size(), out.data(), P) == 0);
                bool ok = true;
                for (size_t k = 0; k < idx.size(); ++k) ok &= std::memcmp(&out[k * P], &mem[idx[k] * P], P) == 0;
                CHECK(ok);
                if (good.size() >= 40) {    // corrupt three blobs: exactly those must be reported
                    std::vector<PageRef> bad = good;
                    Bytes junk(8, 0xFF);
                    bad[1] = PageRef{junk.data(), junk.size()};
                    bad[good.size() / 2] = PageRef{junk.data(), 0};
                    bad[good.size() - 1] = PageRef{junk.data(), junk.size()};
                    CHECK(pool.decompress(bad.data(), bad.size(), out.data(), P) == 3);
                }
            }
        }
    }
    // the same pool used repeatedly, and from several threads at once (calls are serialised)
    PagePool pool(4);
    Bytes mem(200 * P);
    for (auto& c : mem) c = uint8_t(rng() % 5);
    std::vector<std::thread> callers;
    std::atomic<int> okc{0};
    for (int t = 0; t < 4; ++t)
        callers.emplace_back([&] {
            const size_t stride = page_encode_bound(P);
            std::vector<uint8_t> enc(200 * stride);
            std::vector<size_t> len(200);
            for (int rep = 0; rep < 20; ++rep) {
                pool.compress(mem.data(), P, 200, enc.data(), stride, len.data(), 1);
                std::vector<PageRef> refs;
                for (size_t i = 0; i < 200; ++i) if (len[i]) refs.push_back(PageRef{&enc[i * stride], len[i]});
                Bytes out(refs.size() * P);
                if (pool.decompress(refs.data(), refs.size(), out.data(), P) == 0) ++okc;
            }
        });
    for (auto& c : callers) c.join();
    CHECK(okc == 80);
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
    test_page3();
    test_page3_mutation();
    test_cache_concurrency();
    test_pool();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
