// AMDC-F block-decoder benchmark. Build the SAME file against two versions of src/fast.cpp (same flags, same machine)
// and compare. Two workloads:
//  1. synthetic: blocks that END in a short-offset (2..15) match with only the mandatory 5-byte literal tail, i.e. the
//     "tight output + short period" case, decoded into an exact-size buffer.
//  2. pages: a file split into 4 KiB pages, each compressed (level 1) and decoded in random order, as a RAM cache would.
// usage: decode_bench [FILE ...]      (no FILE: synthetic only)
#include "../src/fast.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace amdc1::fast;
using Clock = std::chrono::steady_clock;
static double now() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

static void put_len(Bytes& b, size_t n) { while (n >= 255) { b.push_back(255); n -= 255; } b.push_back(uint8_t(n)); }

// [offset literals][match offset,mlen][5 literals]
static Bytes make_block(size_t offset, size_t mlen, size_t tail, std::mt19937& rng) {
    Bytes b;
    const size_t mc = mlen - 4;
    b.push_back(uint8_t((std::min<size_t>(15, offset) << 4) | std::min<size_t>(15, mc)));
    if (offset >= 15) put_len(b, offset - 15);
    for (size_t i = 0; i < offset; ++i) b.push_back(uint8_t(rng()));
    b.push_back(uint8_t(offset)); b.push_back(uint8_t(offset >> 8));
    if (mc >= 15) put_len(b, mc - 15);
    b.push_back(uint8_t(tail << 4));
    for (size_t i = 0; i < tail; ++i) b.push_back(uint8_t(rng()));
    return b;
}

template <class F>
static double best_rate(size_t bytes_per_pass, double min_secs, int trials, F&& pass) {
    double best = 0;
    for (int t = 0; t < trials; ++t) {
        size_t passes = 0; const double t0 = now();
        while (now() - t0 < min_secs) { pass(); ++passes; }
        best = std::max(best, double(bytes_per_pass) * double(passes) / (now() - t0) / 1e6);
    }
    return best;
}

int main(int argc, char** argv) {
    std::mt19937 rng(42);
    // ---- 1. synthetic tight short-period ----
    {
        std::vector<Bytes> blocks; std::vector<size_t> outlen; size_t total = 0;
        for (size_t offset = 2; offset <= 15; ++offset)
            for (size_t mlen : {4, 5, 6, 8, 12, 16, 24, 31, 64, 100, 127}) {
                blocks.push_back(make_block(offset, mlen, 5, rng));
                outlen.push_back(offset + mlen + 5); total += outlen.back();
            }
        Bytes out(512);
        volatile uint8_t sink = 0;
        auto pass = [&] { for (size_t i = 0; i < blocks.size(); ++i) { block_decompress(blocks[i].data(), blocks[i].size(), out.data(), outlen[i]); sink += out[0]; } };
        const double r = best_rate(total, 0.4, 7, pass);
        std::printf("synthetic tight short-period (%zu blocks, offsets 2-15, mlen 4-127): %8.0f MB/s\n", blocks.size(), r);
    }
    // ---- 2. real files as 4 KiB pages ----
    for (int a = 1; a < argc; ++a) {
        Bytes mem; { FILE* f = std::fopen(argv[a], "rb"); if (!f) { std::perror(argv[a]); return 1; } uint8_t buf[1 << 16]; size_t r; while ((r = std::fread(buf, 1, sizeof buf, f)) > 0) mem.insert(mem.end(), buf, buf + r); std::fclose(f); }
        const size_t P = 4096, np = mem.size() / P;
        std::vector<Bytes> blobs(np); Bytes tmp(block_bound(P));
        for (size_t i = 0; i < np; ++i) { size_t n = block_compress(mem.data() + i * P, P, tmp.data(), 1); blobs[i].assign(tmp.begin(), tmp.begin() + n); }
        std::vector<size_t> ord(np); for (size_t i = 0; i < np; ++i) ord[i] = i;
        std::shuffle(ord.begin(), ord.end(), std::mt19937_64(1));
        Bytes out(P); volatile uint8_t sink = 0;
        auto pass = [&] { for (size_t i : ord) { if (!block_decompress(blobs[i].data(), blobs[i].size(), out.data(), P)) std::abort(); sink += out[0]; } };
        const double r = best_rate(np * P, 0.5, 7, pass);
        std::printf("pages %-40s (%zu x 4 KiB): %8.0f MB/s\n", argv[a], np, r);
    }
}
