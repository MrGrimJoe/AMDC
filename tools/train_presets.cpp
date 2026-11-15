// Trains the built-in Huffman code tables of the page codec (src/presets.hpp).
//   build:  g++ -std=c++17 -O2 -DAMDC_TRAIN tools/train_presets.cpp -o build/train_presets
//   usage:  build/train_presets NUM_LIT NUM_TOK NUM_OFF NUM_EXT OUT.hpp [-s STRIDE] FILE...
// Every full 4 KiB page of every input file is parsed the way the page codec parses it, the literal bytes, token bytes,
// offset bytes and length-extension bytes are histogrammed per page, and the pages are clustered by coding cost (k-means where the "distance"
// is the bits a page needs under a code). A page is assigned to a preset only when that beats coding the page with
// its own code (header included), so presets are built from the pages they actually help.
// Train on data that is NOT your test data, and on a mix that looks like the memory you want to compress.
#include "../src/fast.cpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <numeric>
#include <vector>

using namespace amdc1::fast;

struct Hist { uint32_t f[256]; uint32_t n; double base; };   // base = bits the page costs without any preset

static double custom_bits(const Hist& h) {
    if (h.n < 32) return 8.0 * h.n;
    uint8_t len[256];
    if (!huff_lengths(h.f, len)) return 8.0 * h.n;
    uint64_t bits = 0;
    for (int i = 0; i < 256; ++i) bits += uint64_t(h.f[i]) * len[i];
    return std::min(8.0 * h.n, double(huf_header_size(len) * 8 + 64 + 16 + bits));
}
static double preset_bits(const Hist& h, const uint8_t* len) {
    uint64_t bits = 0;
    for (int i = 0; i < 256; ++i) bits += uint64_t(h.f[i]) * len[i];
    return 72.0 + 8.0 + double(bits);                       // 8 stream-length bytes + id/flush slack
}
static void make_code(const std::vector<double>& sum, uint8_t* len) {
    double total = 0; for (double v : sum) total += v;
    uint32_t f[256];
    const double scale = total > 4e9 ? 4e9 / total : 1.0;
    const double floor_ = std::max(1.0, total * scale / (256.0 * 256.0));
    for (int i = 0; i < 256; ++i) f[i] = uint32_t(sum[i] * scale + floor_);
    huff_lengths(f, len);
}

static std::vector<uint8_t> train(std::vector<Hist>& H, int K, const char* what) {
    std::vector<uint8_t> codes(size_t(K) * 256, 0);
    if (K == 0 || H.empty()) return codes;
    for (auto& h : H) h.base = custom_bits(h);
    std::vector<double> cur(H.size());                       // cost of each page under the best choice so far
    for (size_t i = 0; i < H.size(); ++i) cur[i] = H[i].base;
    // seeding: first preset from everything, then the page that is served worst
    for (int k = 0; k < K; ++k) {
        std::vector<double> sum(256, 0.0);
        if (k == 0) { for (auto& h : H) for (int i = 0; i < 256; ++i) sum[i] += h.f[i]; }
        else {
            size_t worst = 0; double wv = -1;
            for (size_t i = 0; i < H.size(); ++i) {
                if (H[i].n < 64) continue;
                double ent = 0; for (int s = 0; s < 256; ++s) if (H[i].f[s]) ent += H[i].f[s] * std::log2(double(H[i].n) / H[i].f[s]);
                const double gap = (cur[i] - ent) * (H[i].n / 256.0 + 1);   // favour big, badly served pages
                if (gap > wv) { wv = gap; worst = i; }
            }
            for (int i = 0; i < 256; ++i) sum[i] = H[worst].f[i];
        }
        make_code(sum, &codes[size_t(k) * 256]);
        for (size_t i = 0; i < H.size(); ++i) cur[i] = std::min(cur[i], preset_bits(H[i], &codes[size_t(k) * 256]));
    }
    // Lloyd iterations
    std::vector<int> asg(H.size());
    for (int it = 0; it < 14; ++it) {
        double total = 0; size_t used = 0;
        std::vector<std::vector<double>> sums(K, std::vector<double>(256, 0.0));
        std::vector<size_t> cnt(K, 0);
        for (size_t i = 0; i < H.size(); ++i) {
            int best = -1; double bc = H[i].base;
            for (int k = 0; k < K; ++k) { const double c = preset_bits(H[i], &codes[size_t(k) * 256]); if (c < bc) { bc = c; best = k; } }
            asg[i] = best; total += bc;
            if (best >= 0) { ++cnt[best]; ++used; for (int s = 0; s < 256; ++s) sums[best][s] += H[i].f[s]; }
        }
        double basetot = 0; for (auto& h : H) basetot += h.base;
        std::fprintf(stderr, "  %s iter %2d: %.2f MB -> %.2f MB (%.1f%% saved), %zu/%zu pages use a preset\n", what, it,
                     basetot / 8e6, total / 8e6, 100.0 * (1 - total / basetot), used, H.size());
        for (int k = 0; k < K; ++k) if (cnt[k]) make_code(sums[k], &codes[size_t(k) * 256]);
    }
    return codes;
}

int main(int argc, char** argv) {
    if (argc < 7) { std::fprintf(stderr, "usage: train_presets NUM_LIT NUM_TOK NUM_OFF NUM_EXT OUT.hpp [-s STRIDE] FILE...\n"); return 2; }
    int K[4];
    for (int b = 0; b < 4; ++b) { K[b] = std::atoi(argv[1 + b]); if (K[b] < 0 || K[b] > 16) { std::fprintf(stderr, "preset counts must be 0..16\n"); return 2; } }
    const char* outp = argv[5];
    size_t stride = 1;
    std::vector<const char*> files;
    for (int a = 6; a < argc; ++a) { if (!std::strcmp(argv[a], "-s") && a + 1 < argc) stride = size_t(std::atoi(argv[++a])); else files.push_back(argv[a]); }
    std::vector<Hist> H[4];
    size_t pages = 0, kept = 0;
    for (const char* fn : files) {
        std::ifstream f(fn, std::ios::binary);
        std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
        for (size_t p = 0; (p + 1) * 4096 <= d.size(); ++p) {
            ++pages;
            if (p % stride) continue;
            const uint8_t* pg = &d[p * 4096];
            if (all_same_bytes(pg, 4096)) continue;
            uint8_t lz[9000], lits[kP4Max], tok[kP4Max], off[kP4Max], ext[kP4Max];
            const size_t c = page2_compress(pg, 4096, lz, 2);
            if (c <= 128) continue;                          // such pages never reach the Huffman stage
            size_t n[4];
            p4_split(lz, c, lits, n[0], tok, n[1], off, n[2], ext, n[3]);
            ++kept;
            const uint8_t* const src[4] = {lits, tok, off, ext};
            for (int b = 0; b < 4; ++b) {
                Hist h{};
                h.n = uint32_t(n[b]);
                for (size_t i = 0; i < n[b]; ++i) h.f[src[b][i]]++;
                if (n[b] >= 32) H[b].push_back(h);
            }
        }
    }
    const char* names[4] = {"literals", "tokens", "offsets", "extensions"};
    std::fprintf(stderr, "%zu pages read, %zu used\n", pages, kept);
    std::vector<uint8_t> codes[4];
    for (int b = 0; b < 4; ++b) { std::fprintf(stderr, "%s: %zu histograms\n", names[b], H[b].size()); codes[b] = train(H[b], K[b], names[b]); }
    FILE* o = std::fopen(outp, "w");
    if (!o) { std::perror(outp); return 1; }
    std::fprintf(o, "// Built-in Huffman code tables for the page codec. Generated by tools/train_presets.cpp: %d literal, %d token, %d offset, %d extension tables.\n"
                    "// Each row is the code length (1..10) of byte values 0..255; every row is a complete prefix code.\n#pragma once\n#include <cstdint>\n"
                    "namespace amdc1 { namespace fast {\nconstexpr int kNumLitPresets = %d, kNumTokPresets = %d, kNumOffPresets = %d, kNumExtPresets = %d;\n",
                 K[0], K[1], K[2], K[3], K[0], K[1], K[2], K[3]);
    const char* vars[4] = {"kLitPresetLen", "kTokPresetLen", "kOffPresetLen", "kExtPresetLen"};
    for (int b = 0; b < 4; ++b) {
        std::fprintf(o, "static const uint8_t %s[%d][256] = {\n", vars[b], K[b] ? K[b] : 1);
        if (!K[b]) std::fprintf(o, "{0}\n");
        for (int k = 0; k < K[b]; ++k) {
            std::fprintf(o, "{");
            for (int i = 0; i < 256; ++i) std::fprintf(o, "%d%s", codes[b][size_t(k) * 256 + i], i < 255 ? "," : "");
            std::fprintf(o, "}%s\n", k + 1 < K[b] ? "," : "");
        }
        std::fprintf(o, "};\n");
    }
    std::fprintf(o, "} }\n");
    std::fclose(o);
    return 0;
}
