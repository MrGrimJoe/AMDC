// Context-mixing core (format v4): nibble-local hashed context slots with check bytes, 15 context models (12 with
// bit-history states mapped through adaptive state maps), two match models, four first-layer mixers + learned second
// layer, two APM/SSE stages,
// 32-bit binary arithmetic coder with 16-bit probabilities.
// Integer-only; must stay bit-exact with reference/amdc1/cm.py.
#include <algorithm>
#include <array>
#include <cstring>

#include "amdc1.hpp"

namespace amdc1 {
namespace {

constexpr uint64_t K64 = 0x9E3779B97F4A7C15ull;
constexpr uint64_t K2 = 0xD6E8FEB86659FD93ull;
constexpr uint64_t KB = 0x9E3779B1ull;
constexpr int NM = 15;                 // context models
constexpr int NMM = 2;                 // match models
constexpr int LIMS[NM] = {250, 250, 250, 120, 60, 60, 60, 60, 120, 120, 120, 120, 120, 120, 120};
constexpr bool INDM[NM] = {false, false, false, true, true, true, true, true, true, true, true, true, true, true, true};
constexpr int MMIN[NMM] = {5, 12};
constexpr int MVER[NMM] = {32, 48};
constexpr int INITW = 1228;            // 0.15 in 1.0 = 2^13
constexpr int LRMIN = 8, LRBOOST = 60, LRHALF = 8192;   // mixer learning rate (1/8 units): LRMIN + LRBOOST*LRHALF/(LRHALF+t)
constexpr int LRLITE = 48;             // lite profile: fixed rate (6.0 in 1/8 units)
constexpr int LRF = 1;                 // second-layer learning rate
constexpr int SMLIM = 255;             // state-map adaptation limit

#ifndef AMDC_ABLATE_MASK
#define AMDC_ABLATE_MASK 0
#endif
// Model profiles. Lite = 6 context models (direct counters only, no bit histories) + 1 match model + 2 mixers + 1 APM (fast); Full = everything.
constexpr int ACT_FULL[NM] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
constexpr int ACT_LITE[6] = {1, 2, 3, 4, 6, 9};

struct Tables {
    int sq[4096];  // index d+2048
    int st[4096];
    int recip[256];
    static int squash(int d) {
        static const int T[33] = {1, 2, 3, 6, 10, 16, 27, 45, 73, 120, 194, 310, 488, 747, 1101, 1546, 2047,
                                  2549, 2994, 3348, 3607, 3785, 3901, 3975, 4022, 4050, 4068, 4079, 4085,
                                  4089, 4092, 4093, 4094};
        if (d > 2047) return 4095;
        if (d < -2047) return 0;
        int w = d & 127, i = (d >> 7) + 16;
        return (T[i] * (128 - w) + T[i + 1] * w + 64) >> 7;
    }
    Tables() {
        for (int d = -2048; d < 2048; ++d) sq[d + 2048] = squash(d);
        int pi = 0;
        for (int x = -2047; x < 2048; ++x) {
            int v = squash(x);
            for (int i = pi; i <= v; ++i) st[i] = x;
            pi = v + 1;
        }
        for (int i = pi; i < 4096; ++i) st[i] = 2047;
        for (int n = 0; n < 256; ++n) recip[n] = (65536 * 2) / (2 * n + 3);
    }
};
const Tables TB;

// Bit-history states: (n0, n1, last bit) with bounded counts; the opposite count is discounted on each observation.
// Generated breadth-first from the empty history so the numbering is identical in every implementation (229 states).
struct Hist {
    uint8_t nex[256][2] = {};
    uint8_t sn[256] = {};       // n0 + n1
    uint16_t p0[256] = {};      // initial probability of a 1 for the state map
    int count = 0;
    static void step(int& n0, int& n1, int& last, int b) {
        static const int LIMT[6] = {41, 24, 12, 8, 6, 5};
        if (b) { ++n1; if (n0 > 2) n0 = 2 + (n0 - 2) / 2; } else { ++n0; if (n1 > 2) n1 = 2 + (n1 - 2) / 2; }
        const int m = std::min(n0, n1), lim = m < 6 ? LIMT[m] : 5;
        if (n0 > lim) n0 = lim;
        if (n1 > lim) n1 = lim;
        last = (n0 > 0 && n1 > 0) ? b : 0;
    }
    Hist() {
        int a0[256], a1[256], al[256];
        a0[0] = a1[0] = al[0] = 0;
        count = 1;
        for (int q = 0; q < count; ++q)
            for (int b = 0; b < 2; ++b) {
                int n0 = a0[q], n1 = a1[q], l = al[q];
                step(n0, n1, l, b);
                int f = -1;
                for (int k = 0; k < count; ++k)
                    if (a0[k] == n0 && a1[k] == n1 && al[k] == l) { f = k; break; }
                if (f < 0) { f = count++; a0[f] = n0; a1[f] = n1; al[f] = l; }
                nex[q][b] = uint8_t(f);
            }
        for (int q = 0; q < count; ++q) {
            sn[q] = uint8_t(a0[q] + a1[q]);
            p0[q] = uint16_t(((2 * a1[q] + 1) * 65535) / (2 * (a0[q] + a1[q]) + 2));
        }
    }
};
const Hist HS;

inline int bitlen(uint64_t v) { int b = 0; while (v) { ++b; v >>= 1; } return b; }

template <bool ENC, bool LITE>
Bytes run(size_t n, uint32_t s, int membits, const uint8_t* in, const uint8_t* pay, size_t plen) {
    if (n == 0) return {};
    constexpr int NA = LITE ? 6 : NM;
    const int* const ACT = LITE ? ACT_LITE : ACT_FULL;
    constexpr int NMMA = LITE ? 1 : NMM;
    constexpr int NIN = LITE ? 16 : 32;     // padded input vector (SIMD-friendly)
    // Input lanes: stretch(P) of each active model, then match inputs, then bit-history inputs, then the bias.
    int dlane[NM], ilane[NM];
    for (int i = 0; i < NM; ++i) dlane[i] = ilane[i] = -1;
    int nl = 0;
    for (int a = 0; a < NA; ++a) dlane[ACT[a]] = nl++;
    const int mlane0 = nl;
    nl += NMMA;
    for (int a = 0; a < NA; ++a)
        if (!LITE && INDM[ACT[a]]) ilane[ACT[a]] = nl++;
    const int blane = nl;
    const int tbfull = std::min(membits, std::max(16, bitlen(uint64_t(n) * 8) + 1));
    // per-model slot-table size (log2 slots): low orders need far less
    int tbm[NM];
    for (int i = 0; i < NM; ++i) tbm[i] = tbfull;
    tbm[0] = std::min(tbfull, 12);
    tbm[1] = std::min(tbfull, 19);
    int shm[NM];
    uint64_t maskm[NM];
    for (int i = 0; i < NM; ++i) { shm[i] = 64 - tbm[i]; maskm[i] = (uint64_t(1) << tbm[i]) - 1; }
    const int mb = std::min(22, std::max(12, bitlen(n) + 1));
    const int msh = 64 - mb;

    std::array<std::vector<uint32_t>, NM> S;
    for (int a = 0; a < NA; ++a) S[ACT[a]].assign(size_t(1) << tbm[ACT[a]], 32768u);
    std::vector<uint16_t> SMP(size_t(NM) * 8 * 256);
    std::vector<uint8_t> SMN(size_t(NM) * 8 * 256, 0);
    for (int i = 0; i < NM; ++i)
        for (int b = 0; b < 8; ++b)
            for (int q = 0; q < 256; ++q) SMP[(size_t(i) * 8 + b) * 256 + q] = HS.p0[q];
    std::vector<uint32_t> MT[NMM];
    for (int j = 0; j < NMMA; ++j) MT[j].assign(size_t(1) << mb, 0);
    constexpr int NSET1 = 512, NSET2 = 256, NSET3 = 256, NSET4 = 128;
    int16_t winit[NIN] = {};
    for (int k = 0; k < mlane0 + NMMA; ++k) winit[k] = INITW;
    for (int k = mlane0 + NMMA; k < blane; ++k) winit[k] = INITW / 2;
    std::vector<int16_t> W1(size_t(NSET1) * NIN), W2(size_t(NSET2) * NIN);
    std::vector<int16_t> W3(size_t(LITE ? 1 : NSET3) * NIN), W4(size_t(LITE ? 1 : NSET4) * NIN);
    auto initw = [&](std::vector<int16_t>& w) {
        for (size_t i = 0; i < w.size() / NIN; ++i) std::memcpy(&w[i * NIN], winit, sizeof(winit));
    };
    initw(W1); initw(W2); initw(W3); initw(W4);
    std::vector<int64_t> WF(size_t(256) * 4, 16384);
    std::vector<uint16_t> A(size_t(65536) * 33), A2(size_t(LITE ? 1 : 65536) * 33);
    for (size_t i = 0; i < 65536; ++i)
        for (int j = 0; j < 33; ++j) {
            A[i * 33 + j] = uint16_t(std::min(65535, TB.squash((j - 16) * 128) * 16));
            if (!LITE) A2[i * 33 + j] = A[i * 33 + j];
        }
    int MP[NMM][33];
    for (auto& a : MP) for (int& v : a) v = 49152;

    std::vector<uint8_t> buf(n, 0);
    if (ENC) std::memcpy(buf.data(), in, n);
    Bytes out;
    uint32_t x1 = 0, x2 = 0xFFFFFFFFu, x = 0;
    size_t pos = 0;
    if (!ENC) {
        for (int i = 0; i < 4; ++i) x = (x << 8) | (plen > size_t(i) ? pay[i] : 0);
        pos = 4;
    }
    uint64_t c8 = 0;
    uint32_t wh = 0, pwh = 0, ppwh = 0;
    size_t ls = 0, pls = 0;                  // start of the current and the previous line
    size_t mptr[NMM] = {0, 0}, mlen[NMM] = {0, 0};

    for (size_t t = 0; t < n; ++t) {
        const uint32_t c1 = uint32_t(c8 & 0xFF);
        const uint32_t c2 = uint32_t((c8 >> 8) & 0xFF);
        for (int j = 0; j < NMMA; ++j) {
            if (t) {
                if (mlen[j] && buf[mptr[j]] == c1) {
                    if (mlen[j] < 65535) ++mlen[j];
                    ++mptr[j];
                } else {
                    mlen[j] = 0;
                }
                if (t >= size_t(MMIN[j])) {
                    uint64_t hh;
                    if (j == 0) hh = (c8 & 0xFFFFFFFFFFull) * K64;
                    else {
                        hh = 0;
                        for (int k = 1; k <= MMIN[j]; ++k) hh = (hh + buf[t - k] + 1) * K2;
                        hh *= K64;
                    }
                    size_t h = size_t(hh >> msh);
                    if (!mlen[j]) {
                        size_t cand = MT[j][h];
                        if (cand) {
                            size_t ln = 0;
                            while (ln < size_t(MVER[j]) && cand >= ln + 1 && buf[cand - 1 - ln] == buf[t - 1 - ln]) ++ln;
                            if (ln >= size_t(MMIN[j])) { mlen[j] = ln; mptr[j] = cand; }
                        }
                    }
                    MT[j][h] = uint32_t(t);
                }
            }
        }
        int pb[NMM];
        for (int j = 0; j < NMM; ++j) pb[j] = mlen[j] ? buf[mptr[j]] : -1;

        uint64_t cx9, cx10, cx11, cx12, cx13, cx14;
        if (s) {
            const uint64_t a = t >= s ? buf[t - s] : 0;
            const uint64_t b2 = t >= 2 * uint64_t(s) ? buf[t - 2 * uint64_t(s)] : 0;
            const uint64_t b3 = t >= 3 * uint64_t(s) ? buf[t - 3 * uint64_t(s)] : 0;
            const uint64_t al = t >= uint64_t(s) + 1 ? buf[t - s - 1] : 0;
            const uint64_t ar = (s >= 2 && t + 1 >= s) ? buf[t - s + 1] : 0;
            cx9 = (a << 8) | c1;
            cx10 = (a << 8) | b2;
            cx11 = (a << 16) | (al << 8) | c1;
            cx12 = (a << 16) | (b2 << 8) | b3;
            cx13 = (al << 16) | (a << 8) | ar;
            cx14 = (uint64_t(c1) << 16) | (uint64_t(c2) << 8) | a;
        } else {
            const size_t col = t - ls;
            const uint64_t above = (pls + col < ls) ? buf[pls + col] : 0;
            cx9 = wh ? ((uint64_t(wh) << 8) | c1) : (uint64_t(c1) | 0x1000000000ull);
            cx10 = (c8 >> 8) & 0xFFFF;
            cx11 = wh ? ((uint64_t(pwh) << 32) | wh) : ((uint64_t(pwh) << 8) | c1 | 0x2000000000000ull);
            cx12 = ((uint64_t(ppwh) << 32) | pwh) * K2 + wh;
            cx13 = (above << 8) | c1 | 0x3000000ull;
            cx14 = (c8 >> 8) & 0xFFFFFF;
        }
        const uint64_t ctxs[NM] = {0, c1, c8 & 0xFFFF, c8 & 0xFFFFFF, c8 & 0xFFFFFFFFull, c8 & 0xFFFFFFFFFFull,
                                   c8 & 0xFFFFFFFFFFFFull, c8, (c8 & 0xFF) | ((c8 >> 8) & 0xFF00), cx9, cx10, cx11,
                                   cx12, cx13, cx14};
        uint64_t hbase[NM] = {};
        for (int a = 0; a < NA; ++a) { const int i = ACT[a]; hbase[i] = (ctxs[i] + 1 + uint64_t(i) * 0x10001) * K64; }
        uint64_t nbase[NM] = {};     // slot-group base index
        uint32_t chk[NM] = {};
        const int32_t lrq = LITE ? LRLITE : LRMIN + int((int64_t(LRBOOST) * LRHALF) / (LRHALF + int64_t(t)));

        uint32_t c0 = 1;
        const uint32_t byte_in = ENC ? buf[t] : 0;
        for (int bp = 7; bp >= 0; --bp) {
            if (bp == 7 || bp == 3) {
                for (int a = 0; a < NA; ++a) {
                    const int i = ACT[a];
                    uint64_t xh = bp == 7 ? hbase[i] : (hbase[i] + uint64_t(c0) * K2) * K64;
                    nbase[i] = (xh >> shm[i]) & ~uint64_t(15);
                    chk[i] = uint32_t((xh >> (shm[i] - 8)) & 0xFF);
                }
            }
            const uint32_t node = bp >= 4 ? c0 : ((c0 & ((1u << (3 - bp)) - 1)) | (1u << (3 - bp)));
            uint32_t* cp[NM];
            int sidx[NM];
            int16_t xs[NIN] = {};
            for (int a = 0; a < NA; ++a) {
                const int i = ACT[a];
                uint64_t idx = (nbase[i] + node) & maskm[i];
                uint32_t* c = &S[i][idx];
                if ((*c >> 24) != chk[i]) *c = 32768u | (chk[i] << 24);
                cp[i] = c;
                xs[dlane[i]] = int16_t(TB.st[(*c & 0xFFFF) >> 4]);
                if (!LITE && INDM[i]) {
                    sidx[i] = (i * 8 + bp) * 256 + int((*c >> 16) & 0xFF);
                    xs[ilane[i]] = int16_t(TB.st[SMP[sidx[i]] >> 4]);
                }
            }
            int flag[NMM] = {0, 0}, eb[NMM] = {0, 0}, mi[NMM] = {0, 0};
            for (int j = 0; j < NMMA; ++j) {
                if (mlen[j] && (uint32_t(pb[j] | 256) >> (bp + 1)) == c0) {
                    eb[j] = (pb[j] >> bp) & 1;
                    mi[j] = mlen[j] < 16 ? int(mlen[j]) : std::min(16 + int((mlen[j] - 16) >> 2), 32);
                    int sm = TB.st[MP[j][mi[j]] >> 4];
                    xs[mlane0 + j] = int16_t(eb[j] ? sm : -sm);
                    flag[j] = 1;
                }
            }
            xs[blane] = 256;
            if (AMDC_ABLATE_MASK)
                for (int k = 0; k < NIN; ++k)
                    if ((AMDC_ABLATE_MASK >> k) & 1) xs[k] = 0;
            int16_t* w1 = &W1[size_t(c0 * 2 + flag[0]) * NIN];
            int16_t* w2 = &W2[size_t(c1) * NIN];
            int16_t* w3 = &W3[size_t(LITE ? 0 : c2) * NIN];
            uint32_t seenmask = 0;
            if (!LITE)
                for (int k = 2; k <= 7; ++k) seenmask = seenmask * 2 + (((*cp[k] >> 16) & 0xFF) != 0 ? 1u : 0u);
            int16_t* w4 = &W4[size_t(LITE ? 0 : seenmask * 2 + uint32_t(flag[0])) * NIN];
            int32_t d1 = 0, d2 = 0, d3 = 0, d4 = 0;
            for (int k = 0; k < NIN; ++k) {
                d1 += int32_t(w1[k]) * xs[k];
                d2 += int32_t(w2[k]) * xs[k];
                if (!LITE) { d3 += int32_t(w3[k]) * xs[k]; d4 += int32_t(w4[k]) * xs[k]; }
            }
            d1 >>= 13; d2 >>= 13; d3 >>= 13; d4 >>= 13;
            auto clip = [](int32_t v) { return v > 2047 ? 2047 : v < -2047 ? -2047 : v; };
            d1 = clip(d1); d2 = clip(d2);
            int32_t dm;
            int64_t* wf = &WF[size_t(c0) * 4];
            if (!LITE) {
                d3 = clip(d3); d4 = clip(d4);
                dm = clip(int32_t((wf[0] * d1 + wf[1] * d2 + wf[2] * d3 + wf[3] * d4) >> 16));
            } else {
                dm = (d1 + d2) >> 1;
            }
            const int p1 = TB.sq[d1 + 2048], p2 = TB.sq[d2 + 2048];
            const int p3 = LITE ? 0 : TB.sq[d3 + 2048], p4 = LITE ? 0 : TB.sq[d4 + 2048];
            const int pm = TB.sq[dm + 2048];
            const int av = int(dm + 2048), aw = av & 127;
            uint16_t* ap = &A[(size_t(c1) << 8 | c0) * 33 + (av >> 7)];
            int pa = (ap[0] * (128 - aw) + ap[1] * aw) >> 7;
            uint16_t* ap2 = nullptr;
            int p;
            if (!LITE) {
                const uint32_t h2 = uint32_t((((c8 & 0xFFFF) << 8 | c0) * K64) >> 48);
                ap2 = &A2[size_t(h2) * 33 + (av >> 7)];
                int pa2 = (ap2[0] * (128 - aw) + ap2[1] * aw) >> 7;
                p = (pm * 16 * 2 + 3 * pa + 3 * pa2 + 4) >> 3;
            } else {
                p = (pm * 16 + 3 * pa + 2) >> 2;
            }
            if (p < 4) p = 4;
            if (p > 65532) p = 65532;

            uint32_t xmid = x1 + uint32_t((uint64_t(x2 - x1) * uint64_t(p)) >> 16);
            int bit;
            if (ENC) bit = (byte_in >> bp) & 1;
            else bit = x <= xmid ? 1 : 0;
            if (bit) x2 = xmid; else x1 = xmid + 1;
            while (((x1 ^ x2) & 0xFF000000u) == 0) {
                if (ENC) {
                    out.push_back(uint8_t(x2 >> 24));
                } else {
                    uint32_t nb = pos < plen ? pay[pos] : 0;
                    ++pos;
                    x = (x << 8) | nb;
                }
                x1 <<= 8;
                x2 = (x2 << 8) | 255u;
            }

            const int32_t e1 = (((bit << 12) - p1) * lrq) >> 3;
            const int32_t e2 = (((bit << 12) - p2) * lrq) >> 3;
            for (int k = 0; k < NIN; ++k) {
                int32_t a = int32_t(w1[k]) + ((xs[k] * e1 + 32768) >> 16);
                int32_t b = int32_t(w2[k]) + ((xs[k] * e2 + 32768) >> 16);
                w1[k] = int16_t(a > 32767 ? 32767 : a < -32768 ? -32768 : a);
                w2[k] = int16_t(b > 32767 ? 32767 : b < -32768 ? -32768 : b);
            }
            if (!LITE) {
                const int32_t e3 = (((bit << 12) - p3) * lrq) >> 3;
                const int32_t e4 = (((bit << 12) - p4) * lrq) >> 3;
                for (int k = 0; k < NIN; ++k) {
                    int32_t c = int32_t(w3[k]) + ((xs[k] * e3 + 32768) >> 16);
                    int32_t g = int32_t(w4[k]) + ((xs[k] * e4 + 32768) >> 16);
                    w3[k] = int16_t(c > 32767 ? 32767 : c < -32768 ? -32768 : c);
                    w4[k] = int16_t(g > 32767 ? 32767 : g < -32768 ? -32768 : g);
                }
                const int64_t ef = int64_t((bit << 12) - pm) * LRF;
                wf[0] += (int64_t(d1) * ef) >> 14;
                wf[1] += (int64_t(d2) * ef) >> 14;
                wf[2] += (int64_t(d3) * ef) >> 14;
                wf[3] += (int64_t(d4) * ef) >> 14;
            }
            for (int a = 0; a < NA; ++a) {
                const int i = ACT[a];
                uint32_t& cell = *cp[i];
                const int pp = int(cell & 0xFFFF);
                if (!LITE && INDM[i]) {
                    const uint32_t stt = (cell >> 16) & 0xFF;
                    uint32_t cn = HS.sn[stt];
                    if (cn > uint32_t(LIMS[i])) cn = uint32_t(LIMS[i]);
                    const int np = pp + ((((bit << 16) - pp) * TB.recip[cn]) >> 16);
                    cell = (cell & 0xFF000000u) | uint32_t(np) | (uint32_t(HS.nex[stt][bit]) << 16);
                    uint16_t& sp = SMP[sidx[i]];
                    uint8_t& sc = SMN[sidx[i]];
                    const int tg = bit ? 65535 : 0;
                    sp = uint16_t(int(sp) + (((tg - int(sp)) * TB.recip[sc]) >> 16));
                    if (sc < SMLIM) ++sc;
                } else {
                    const uint32_t cn = (cell >> 16) & 0xFF;
                    const int np = pp + ((((bit << 16) - pp) * TB.recip[cn]) >> 16);
                    cell = (cell & 0xFF000000u) | uint32_t(np) | ((cn < uint32_t(LIMS[i]) ? cn + 1 : cn) << 16);
                }
            }
            {
                const int tgt = bit ? 65535 : 0, rnd = bit ? 63 : 0;
                ap[0] = uint16_t(ap[0] + ((tgt - int(ap[0]) + rnd) >> 6));
                ap[1] = uint16_t(ap[1] + ((tgt - int(ap[1]) + rnd) >> 6));
                if (!LITE) {
                    ap2[0] = uint16_t(ap2[0] + ((tgt - int(ap2[0]) + rnd) >> 6));
                    ap2[1] = uint16_t(ap2[1] + ((tgt - int(ap2[1]) + rnd) >> 6));
                }
            }
            for (int j = 0; j < NMMA; ++j)
                if (flag[j]) MP[j][mi[j]] += (((bit == eb[j] ? 1 : 0) << 16) - MP[j][mi[j]]) >> 6;
            c0 = (c0 << 1) | uint32_t(bit);
        }
        const uint32_t byte = c0 & 255;
        if (!ENC) buf[t] = uint8_t(byte);
        c8 = (c8 << 8) | byte;
        if (byte == 10) { pls = ls; ls = t + 1; }
        if (!s) {
            if (byte >= 128 || (byte >= 48 && byte <= 57) || (byte >= 65 && byte <= 90) ||
                (byte >= 97 && byte <= 122)) {
                wh = wh * 0x2F0B4A27u + byte + 1;
            } else {
                if (wh) { ppwh = pwh; pwh = wh; }
                wh = 0;
            }
        }
    }
    if (ENC) {
        for (int sft = 24; sft >= 0; sft -= 8) out.push_back(uint8_t(x1 >> sft));
        return out;
    }
    return buf;
}

}  // namespace

Bytes cm_encode(const uint8_t* data, size_t n, uint32_t s, const CmCfg& cfg) {
    return cfg.lite ? run<true, true>(n, s, cfg.membits, data, nullptr, 0) : run<true, false>(n, s, cfg.membits, data, nullptr, 0);
}
Bytes cm_decode(const uint8_t* payload, size_t plen, size_t n, uint32_t s, const CmCfg& cfg) {
    return cfg.lite ? run<false, true>(n, s, cfg.membits, nullptr, payload, plen) : run<false, false>(n, s, cfg.membits, nullptr, payload, plen);
}

}  // namespace amdc1
