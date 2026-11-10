// Context-mixing core: bitwise logistic mixing + 32-bit binary arithmetic coder.
// Integer-only; must stay bit-exact with reference/amdc1/cm.py.
#include <algorithm>
#include <array>
#include <cstring>

#include "amdc1.hpp"

namespace amdc1 {
namespace {

constexpr uint64_t K64 = 0x9E3779B97F4A7C15ull;
constexpr uint64_t KB = 0x9E3779B1ull;
constexpr int NM = 8;
constexpr int NIN = NM + 2;
constexpr int LIMS[NM] = {250, 250, 250, 120, 60, 60, 120, 120};
constexpr int LR = 12;
constexpr size_t MINM = 5;

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

template <bool ENC>
Bytes run(size_t n, uint32_t s, const uint8_t* in, const uint8_t* pay, size_t plen) {
    if (n == 0) return {};
    const int tb = std::min(22, std::max(16, bitlen(uint64_t(n) * 8) + 1));
    const uint64_t mask = (uint64_t(1) << tb) - 1;
    const int sh = 64 - tb;
    const int mb = std::min(22, std::max(12, bitlen(n) + 1));
    const int msh = 64 - mb;

    std::array<std::vector<uint16_t>, NM> P;
    std::array<std::vector<uint8_t>, NM> C;
    for (int i = 0; i < NM; ++i) {
        P[i].assign(size_t(1) << tb, 32768);
        C[i].assign(size_t(1) << tb, 0);
    }
    std::vector<uint32_t> MT(size_t(1) << mb, 0);
    std::vector<int64_t> W(size_t(512) * NIN);
    for (size_t i = 0; i < 512; ++i) {
        for (int k = 0; k < NM + 1; ++k) W[i * NIN + k] = 13107;
        W[i * NIN + NM + 1] = 0;
    }
    int MP[33];
    for (int& v : MP) v = 49152;

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
    uint32_t wh = 0;
    size_t mptr = 0, mlen = 0;

    for (size_t t = 0; t < n; ++t) {
        const uint32_t c1 = uint32_t(c8 & 0xFF);
        if (t) {
            if (mlen && buf[mptr] == c1) {
                if (mlen < 65535) ++mlen;
                ++mptr;
            } else {
                mlen = 0;
            }
            if (t >= MINM) {
                size_t h = size_t(((c8 & 0xFFFFFFFFFFull) * K64) >> msh);
                if (!mlen) {
                    size_t cand = MT[h];
                    if (cand) {
                        size_t ln = 0;
                        while (ln < 32 && cand >= ln + 1 && buf[cand - 1 - ln] == buf[t - 1 - ln]) ++ln;
                        if (ln >= MINM) { mlen = ln; mptr = cand; }
                    }
                }
                MT[h] = uint32_t(t);
            }
        }
        const int pb = mlen ? buf[mptr] : -1;
        uint64_t cx6, cx7;
        if (s) {
            uint64_t a = t >= s ? buf[t - s] : 0;
            uint64_t b2 = t >= 2 * uint64_t(s) ? buf[t - 2 * uint64_t(s)] : 0;
            cx6 = (a << 8) | c1;
            cx7 = (a << 8) | b2;
        } else {
            cx6 = wh ? ((uint64_t(wh) << 8) | c1) : (uint64_t(c1) | 0x1000000000ull);
            cx7 = (c8 >> 8) & 0xFFFF;
        }
        const uint64_t ctxs[NM] = {0, c1, c8 & 0xFFFF, c8 & 0xFFFFFF, c8 & 0xFFFFFFFFull,
                                   c8 & 0xFFFFFFFFFFFFull, cx6, cx7};
        uint64_t bases[NM];
        for (int i = 0; i < NM; ++i) bases[i] = ((ctxs[i] + 1 + uint64_t(i) * 0x10001) * K64) >> sh;

        uint32_t c0 = 1;
        const uint32_t byte_in = ENC ? buf[t] : 0;
        for (int bp = 7; bp >= 0; --bp) {
            uint64_t idx[NM];
            int xs[NIN];
            for (int i = 0; i < NM; ++i) {
                idx[i] = (bases[i] + uint64_t(c0) * KB) & mask;
                xs[i] = TB.st[P[i][idx[i]] >> 4];
            }
            int flag = 0, eb = 0, mi = 0;
            if (mlen && (uint32_t(pb | 256) >> (bp + 1)) == c0) {
                eb = (pb >> bp) & 1;
                mi = mlen < 16 ? int(mlen) : std::min(16 + int((mlen - 16) >> 2), 32);
                int sm = TB.st[MP[mi] >> 4];
                xs[NM] = eb ? sm : -sm;
                flag = 1;
            } else {
                xs[NM] = 0;
            }
            xs[NM + 1] = 256;
            int64_t* w = &W[size_t(c0 * 2 + flag) * NIN];
            int64_t dot = 0;
            for (int k = 0; k < NIN; ++k) dot += w[k] * xs[k];
            dot >>= 16;
            if (dot > 2047) dot = 2047;
            else if (dot < -2047) dot = -2047;
            int p = TB.sq[dot + 2048];
            if (p < 1) p = 1;

            uint32_t xmid = x1 + uint32_t(((x2 - x1) >> 12) * uint32_t(p));
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

            const int64_t err = int64_t((bit << 12) - p) * LR;
            for (int k = 0; k < NIN; ++k) w[k] += (int64_t(xs[k]) * err) >> 14;
            for (int i = 0; i < NM; ++i) {
                uint8_t& cn = C[i][idx[i]];
                uint16_t& pp = P[i][idx[i]];
                pp = uint16_t(int(pp) + ((((bit << 16) - int(pp)) * TB.recip[cn]) >> 16));
                if (cn < LIMS[i]) ++cn;
            }
            if (flag) MP[mi] += (((bit == eb ? 1 : 0) << 16) - MP[mi]) >> 6;
            c0 = (c0 << 1) | uint32_t(bit);
        }
        const uint32_t byte = c0 & 255;
        if (!ENC) buf[t] = uint8_t(byte);
        c8 = (c8 << 8) | byte;
        if (!s) {
            if (byte >= 128 || (byte >= 48 && byte <= 57) || (byte >= 65 && byte <= 90) ||
                (byte >= 97 && byte <= 122))
                wh = wh * 0x2F0B4A27u + byte + 1;
            else
                wh = 0;
        }
    }
    if (ENC) {
        for (int sft = 24; sft >= 0; sft -= 8) out.push_back(uint8_t(x1 >> sft));
        return out;
    }
    return buf;
}

}  // namespace

Bytes cm_encode(const uint8_t* data, size_t n, uint32_t s) { return run<true>(n, s, data, nullptr, 0); }
Bytes cm_decode(const uint8_t* payload, size_t plen, size_t n, uint32_t s) {
    return run<false>(n, s, nullptr, payload, plen);
}

}  // namespace amdc1
