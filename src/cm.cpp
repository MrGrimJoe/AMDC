// Context-mixing core (format v3): nibble-local hashed context slots with check bytes, 12 context models,
// two match models, three first-layer mixers + learned second layer, two APM/SSE stages,
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
constexpr int NM = 12;
constexpr int NMM = 2;                 // match models
constexpr int NX = NM + NMM;           // stretch inputs from models
constexpr int NIN = 16;                // padded: NX inputs, bias at NX, zero pad (SIMD-friendly)
constexpr int LIMS[NM] = {250, 250, 250, 120, 60, 60, 60, 60, 120, 120, 120, 120};
constexpr int MMIN[NMM] = {5, 12};
constexpr int MVER[NMM] = {32, 48};
constexpr int INITW = 1228;            // 0.15 in 1.0 = 2^13
constexpr int LR6 = 6;                 // error scale (see update)
constexpr int LRF = 3;

#ifndef AMDC_ABLATE_MASK
#define AMDC_ABLATE_MASK 0
#endif
// Model profiles. Lite = 6 context models + 1 match model + 2 mixers + 1 APM (fast); Full = everything.
constexpr int ACT_FULL[NM] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
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

inline int bitlen(uint64_t v) { int b = 0; while (v) { ++b; v >>= 1; } return b; }

template <bool ENC, bool LITE>
Bytes run(size_t n, uint32_t s, int membits, const uint8_t* in, const uint8_t* pay, size_t plen) {
    if (n == 0) return {};
    constexpr int NA = LITE ? 6 : NM;
    const int* const ACT = LITE ? ACT_LITE : ACT_FULL;
    constexpr int NMMA = LITE ? 1 : NMM;
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
    std::vector<uint32_t> MT[NMM];
    for (int j = 0; j < NMMA; ++j) MT[j].assign(size_t(1) << mb, 0);
    constexpr int NSET1 = 512, NSET2 = 256, NSET3 = 256;
    std::vector<int16_t> W1(size_t(NSET1) * NIN), W2(size_t(NSET2) * NIN), W3(size_t(NSET3) * NIN);
    auto initw = [&](std::vector<int16_t>& w, size_t sets) {
        for (size_t i = 0; i < sets; ++i) {
            for (int k = 0; k < NX; ++k) w[i * NIN + k] = INITW;
            for (int k = NX; k < NIN; ++k) w[i * NIN + k] = 0;
        }
    };
    initw(W1, NSET1); initw(W2, NSET2); initw(W3, NSET3);
    std::vector<int64_t> WF(size_t(256) * 3, 21845);
    std::vector<uint16_t> A(size_t(65536) * 33), A2(size_t(65536) * 33);
    for (size_t i = 0; i < 65536; ++i)
        for (int j = 0; j < 33; ++j) {
            A[i * 33 + j] = A2[i * 33 + j] = uint16_t(std::min(65535, TB.squash((j - 16) * 128) * 16));
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
    uint32_t wh = 0, pwh = 0;
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

        uint64_t cx9, cx10, cx11;
        if (s) {
            uint64_t a = t >= s ? buf[t - s] : 0;
            uint64_t b2 = t >= 2 * uint64_t(s) ? buf[t - 2 * uint64_t(s)] : 0;
            uint64_t al = t >= uint64_t(s) + 1 ? buf[t - s - 1] : 0;
            cx9 = (a << 8) | c1;
            cx10 = (a << 8) | b2;
            cx11 = (a << 16) | (al << 8) | c1;
        } else {
            cx9 = wh ? ((uint64_t(wh) << 8) | c1) : (uint64_t(c1) | 0x1000000000ull);
            cx10 = (c8 >> 8) & 0xFFFF;
            cx11 = wh ? ((uint64_t(pwh) << 32) | wh) : ((uint64_t(pwh) << 8) | c1 | 0x2000000000000ull);
        }
        const uint64_t ctxs[NM] = {0, c1, c8 & 0xFFFF, c8 & 0xFFFFFF, c8 & 0xFFFFFFFFull, c8 & 0xFFFFFFFFFFull,
                                   c8 & 0xFFFFFFFFFFFFull, c8, (c8 & 0xFF) | ((c8 >> 8) & 0xFF00), cx9, cx10, cx11};
        uint64_t hbase[NM];
        for (int a = 0; a < NA; ++a) { const int i = ACT[a]; hbase[i] = (ctxs[i] + 1 + uint64_t(i) * 0x10001) * K64; }
        uint64_t nbase[NM] = {};     // slot-group base index (nibble mode)
        uint32_t chk[NM] = {};

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
            int16_t xs[NIN] = {};
            for (int a = 0; a < NA; ++a) {
                const int i = ACT[a];
                uint64_t idx = (nbase[i] + node) & maskm[i];
                uint32_t* c = &S[i][idx];
                if ((*c >> 24) != chk[i]) *c = 32768u | (chk[i] << 24);
                cp[i] = c;
                xs[i] = int16_t(TB.st[(*c & 0xFFFF) >> 4]);
            }
            int flag[NMM] = {0, 0}, eb[NMM] = {0, 0}, mi[NMM] = {0, 0};
            for (int j = 0; j < NMMA; ++j) {
                xs[NM + j] = 0;
                if (mlen[j] && (uint32_t(pb[j] | 256) >> (bp + 1)) == c0) {
                    eb[j] = (pb[j] >> bp) & 1;
                    mi[j] = mlen[j] < 16 ? int(mlen[j]) : std::min(16 + int((mlen[j] - 16) >> 2), 32);
                    int sm = TB.st[MP[j][mi[j]] >> 4];
                    xs[NM + j] = int16_t(eb[j] ? sm : -sm);
                    flag[j] = 1;
                }
            }
            xs[NX] = 256;
            if (AMDC_ABLATE_MASK)
                for (int i = 0; i < NX; ++i)
                    if ((AMDC_ABLATE_MASK >> i) & 1) xs[i] = 0;
            int16_t* w1 = &W1[size_t(c0 * 2 + flag[0]) * NIN];
            int16_t* w2 = &W2[size_t(c1) * NIN];
            int16_t* w3 = &W3[size_t(LITE ? 0 : c2) * NIN];
            int32_t d1 = 0, d2 = 0, d3 = 0;
            for (int k = 0; k < NIN; ++k) {
                d1 += int32_t(w1[k]) * xs[k];
                d2 += int32_t(w2[k]) * xs[k];
                if (!LITE) d3 += int32_t(w3[k]) * xs[k];
            }
            d1 >>= 13; d2 >>= 13; d3 >>= 13;
            auto clip = [](int32_t v) { return v > 2047 ? 2047 : v < -2047 ? -2047 : v; };
            d1 = clip(d1); d2 = clip(d2);
            int32_t dm;
            int64_t* wf = &WF[size_t(c0) * 3];
            if (!LITE) {
                d3 = clip(d3);
                dm = clip(int32_t((wf[0] * d1 + wf[1] * d2 + wf[2] * d3) >> 16));
            } else {
                dm = (d1 + d2) >> 1;
            }
            const int p1 = TB.sq[d1 + 2048], p2 = TB.sq[d2 + 2048], p3 = LITE ? 0 : TB.sq[d3 + 2048];
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

            const int32_t e1 = ((bit << 12) - p1) * LR6;
            const int32_t e2 = ((bit << 12) - p2) * LR6;
            const int32_t e3 = ((bit << 12) - p3) * LR6;
            for (int k = 0; k < NIN; ++k) {
                int32_t a = int32_t(w1[k]) + ((xs[k] * e1 + 32768) >> 16);
                int32_t b = int32_t(w2[k]) + ((xs[k] * e2 + 32768) >> 16);
                w1[k] = int16_t(a > 32767 ? 32767 : a < -32768 ? -32768 : a);
                w2[k] = int16_t(b > 32767 ? 32767 : b < -32768 ? -32768 : b);
            }
            if (!LITE) {
                for (int k = 0; k < NIN; ++k) {
                    int32_t c = int32_t(w3[k]) + ((xs[k] * e3 + 32768) >> 16);
                    w3[k] = int16_t(c > 32767 ? 32767 : c < -32768 ? -32768 : c);
                }
                const int64_t ef = int64_t((bit << 12) - pm) * LRF;
                wf[0] += (int64_t(d1) * ef) >> 14;
                wf[1] += (int64_t(d2) * ef) >> 14;
                wf[2] += (int64_t(d3) * ef) >> 14;
            }
            for (int a = 0; a < NA; ++a) {
                const int i = ACT[a];
                uint32_t& cell = *cp[i];
                const int pp = int(cell & 0xFFFF);
                const uint32_t cn = (cell >> 16) & 0xFF;
                const int np = pp + ((((bit << 16) - pp) * TB.recip[cn]) >> 16);
                cell = (cell & 0xFF000000u) | uint32_t(np) | ((cn < uint32_t(LIMS[i]) ? cn + 1 : cn) << 16);
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
        if (!s) {
            if (byte >= 128 || (byte >= 48 && byte <= 57) || (byte >= 65 && byte <= 90) ||
                (byte >= 97 && byte <= 122)) {
                wh = wh * 0x2F0B4A27u + byte + 1;
            } else {
                if (wh) pwh = wh;
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
