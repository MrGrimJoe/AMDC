// Page-level RAM-compression benchmark: amdcf vs LZ4 / zstd (the codecs Linux zram/zswap use).
// Every page is compressed independently (4 KiB by default), every result is decoded back and compared.
// Pages that are all one byte value are counted as 8 bytes for every codec (zram "same-filled" pages),
// and a page that does not shrink is kept raw (counted as page size), so the ratio is comparable across codecs.
//
// build: see bench/bench_pages.sh  (needs liblz4 and libzstd headers + static libs; amdcf comes from src/fast.cpp)
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "../src/fast.hpp"

#ifdef HAVE_LZ4
#include <lz4.h>
#include <lz4hc.h>
#endif
#ifdef HAVE_ZSTD
#include <zstd.h>
#endif
#ifdef HAVE_ZLIB
#include <zlib.h>
#endif

#include <atomic>
using Clock = std::chrono::steady_clock;
#ifdef HAVE_ZSTD
struct ZCtx { ZSTD_CCtx* c = ZSTD_createCCtx(); ZSTD_DCtx* d = ZSTD_createDCtx(); };
static ZCtx& zctx_get() { static thread_local ZCtx z; return z; }   // one context pair per thread, made on first use
#endif

// runs fn(begin, end) over [0, n) split into `threads` contiguous slices; threads == 1 runs inline
template <class F>
static void run_threads(unsigned threads, size_t n, F fn) {
    if (threads <= 1) { fn(0, n); return; }
    std::vector<std::thread> th;
    for (unsigned k = 0; k < threads; ++k) th.emplace_back([&, k] { fn(n * k / threads, n * (k + 1) / threads); });
    for (auto& t : th) t.join();
}
static double secs(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

struct Codec {
    std::string name;
    std::function<size_t(const uint8_t*, size_t, uint8_t*, size_t)> enc;   // returns 0 on failure
    std::function<bool(const uint8_t*, size_t, uint8_t*, size_t)> dec;
};

static bool same_filled(const uint8_t* p, size_t n) {
    uint64_t pat;
    std::memset(&pat, p[0], 8);
    for (size_t i = 0; i + 8 <= n; i += 8) {
        uint64_t v;
        std::memcpy(&v, p + i, 8);
        if (v != pat) return false;
    }
    return true;
}

int main(int argc, char** argv) {
    size_t page = 4096;
    int reps = 5;
    unsigned threads = 1;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-p" && i + 1 < argc) page = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "-r" && i + 1 < argc) reps = std::atoi(argv[++i]);
        else if (a == "-t" && i + 1 < argc) threads = unsigned(std::atoi(argv[++i]));
        else files.push_back(a);
    }
    if (files.empty()) { std::fprintf(stderr, "usage: bench_pages [-p PAGE] [-r REPS] [-t THREADS] FILE...\n"); return 64; }

    std::vector<Codec> codecs;
    using namespace amdc1::fast;
    for (int lv : {1, 2}) {
        codecs.push_back({"amdcf-L" + std::to_string(lv),
            [lv, page](const uint8_t* s, size_t n, uint8_t* d, size_t) { return block_compress(s, n, d, lv); },
            [](const uint8_t* s, size_t n, uint8_t* d, size_t on) { return block_decompress(s, n, d, on); }});
    }
    if (page <= kPage2Max)   // public page levels 3..6 = page3_compress levels 1..4
        for (int lv : {1, 2, 3, 4}) {
            codecs.push_back({"amdcf-L" + std::to_string(lv + 2),
                [lv](const uint8_t* s, size_t n, uint8_t* d, size_t) { return page3_compress(s, n, d, lv); },
                [](const uint8_t* s, size_t n, uint8_t* d, size_t on) { return page3_decompress(s, n, d, on); }});
        }
#ifdef HAVE_LZ4
    for (int acc : {1, 4}) {
        codecs.push_back({"lz4-fast" + std::to_string(acc),
            [acc](const uint8_t* s, size_t n, uint8_t* d, size_t cap) { return size_t(LZ4_compress_fast((const char*)s, (char*)d, int(n), int(cap), acc)); },
            [](const uint8_t* s, size_t n, uint8_t* d, size_t on) { return LZ4_decompress_safe((const char*)s, (char*)d, int(n), int(on)) == int(on); }});
    }
    codecs.push_back({"lz4hc-3",
        [](const uint8_t* s, size_t n, uint8_t* d, size_t cap) { return size_t(LZ4_compress_HC((const char*)s, (char*)d, int(n), int(cap), 3)); },
        [](const uint8_t* s, size_t n, uint8_t* d, size_t on) { return LZ4_decompress_safe((const char*)s, (char*)d, int(n), int(on)) == int(on); }});
#endif
#ifdef HAVE_ZSTD
    // contexts are reused across pages, exactly as zswap/zram do; allocating one per page would understate zstd badly
    for (int lv : {-3, 1, 3}) {
        codecs.push_back({"zstd" + std::to_string(lv),
            [lv](const uint8_t* s, size_t n, uint8_t* d, size_t cap) { size_t r = ZSTD_compressCCtx(zctx_get().c, d, cap, s, n, lv); return ZSTD_isError(r) ? 0 : r; },
            [](const uint8_t* s, size_t n, uint8_t* d, size_t on) { size_t r = ZSTD_decompressDCtx(zctx_get().d, d, on, s, n); return !ZSTD_isError(r) && r == on; }});
    }
#endif
#ifdef HAVE_ZLIB
    // deflate level 1 = LZ77 + Huffman, the same family and a similar ratio class as Windows' Xpress-Huffman
    // (which is NOT available off Windows and is not measured here)
    codecs.push_back({"zlib-1",
        [](const uint8_t* s, size_t n, uint8_t* d, size_t cap) { uLongf o = uLongf(cap); return compress2(d, &o, s, uLong(n), 1) == Z_OK ? size_t(o) : size_t(0); },
        [](const uint8_t* s, size_t n, uint8_t* d, size_t on) { uLongf o = uLongf(on); return uncompress(d, &o, s, uLong(n)) == Z_OK && o == on; }});
#endif

    std::printf("threads: %u\n%-14s %-9s %8s %10s %10s\n", threads, "file", "codec", "size%", "comp MB/s", "decomp MB/s");
    std::vector<double> tot_c(codecs.size(), 0), tot_ct(codecs.size(), 0), tot_dt(codecs.size(), 0);
    double tot_raw = 0;
    for (const auto& f : files) {
        std::ifstream in(f, std::ios::binary);
        std::vector<uint8_t> mem((std::istreambuf_iterator<char>(in)), {});
        mem.resize(mem.size() / page * page);
        const size_t np = mem.size() / page;
        if (!np) continue;
        std::vector<char> same(np);
        size_t nsame = 0;
        for (size_t i = 0; i < np; ++i) { same[i] = same_filled(&mem[i * page], page); nsame += same[i]; }
        const size_t cap = page * 2 + 1024;
        std::vector<uint8_t> store(np * cap);
        std::vector<size_t> clen(np);
        std::vector<uint8_t> out(page);
        const std::string base = f.substr(f.find_last_of('/') + 1);
        tot_raw += double(mem.size());
        for (size_t ci = 0; ci < codecs.size(); ++ci) {
            const Codec& c = codecs[ci];
            double best_c = 1e30, best_d = 1e30;
            for (int r = 0; r < reps; ++r) {
                auto t = Clock::now();
                run_threads(threads, np, [&](size_t b, size_t e) {
                    for (size_t i = b; i < e; ++i)
                        clen[i] = same[i] ? 0 : c.enc(&mem[i * page], page, &store[i * cap], cap);
                });
                best_c = std::min(best_c, secs(t));
            }
            double comp = 0;
            for (size_t i = 0; i < np; ++i) comp += same[i] ? 8 : (clen[i] && clen[i] < page ? double(clen[i]) : double(page));
            for (int r = 0; r < reps; ++r) {
                std::atomic<bool> bad{false};
                auto t = Clock::now();
                run_threads(threads, np, [&](size_t b, size_t e) {
                    std::vector<uint8_t> o(page);
                    for (size_t i = b; i < e; ++i) {
                        if (same[i] || !clen[i] || clen[i] >= page) continue;
                        if (!c.dec(&store[i * cap], clen[i], o.data(), page)) bad = true;
                    }
                });
                if (bad) { std::fprintf(stderr, "DECODE FAIL %s\n", c.name.c_str()); return 2; }
                best_d = std::min(best_d, secs(t));
            }
            for (size_t i = 0; i < np; ++i) {   // verify outside the timed region
                if (same[i] || !clen[i] || clen[i] >= page) continue;
                if (!c.dec(&store[i * cap], clen[i], out.data(), page) || std::memcmp(out.data(), &mem[i * page], page) != 0) {
                    std::fprintf(stderr, "MISMATCH %s page %zu\n", c.name.c_str(), i); return 2;
                }
            }
            const double mb = double(mem.size()) / 1e6;
            std::printf("%-14s %-9s %7.2f%% %10.0f %10.0f\n", base.c_str(), c.name.c_str(), 100.0 * comp / double(mem.size()), mb / best_c, mb / best_d);
            tot_c[ci] += comp; tot_ct[ci] += best_c; tot_dt[ci] += best_d;
        }
    }
    std::printf("\nTOTAL (%.0f MB)\n", tot_raw / 1e6);
    for (size_t ci = 0; ci < codecs.size(); ++ci)
        std::printf("%-14s %-9s %7.2f%% %10.0f %10.0f\n", "ALL", codecs[ci].name.c_str(), 100.0 * tot_c[ci] / tot_raw, tot_raw / 1e6 / tot_ct[ci], tot_raw / 1e6 / tot_dt[ci]);
    return 0;
}
