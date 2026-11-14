#include "bwtcodec.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>

#include "bwt.hpp"

namespace amdc1 {
namespace bwtc {
namespace {

constexpr uint8_t VERSION = 1;
constexpr uint64_t MAX_RATIO = 16384;

// ---------------------------------------------------------------- tables shared with the CM codec (same squash)
struct Tabs {
    int sq[4096];
    int st[4096];
    static int squash(int d) {
        static const int T[33] = {1, 2, 3, 6, 10, 16, 27, 45, 73, 120, 194, 310, 488, 747, 1101, 1546, 2047,
                                  2549, 2994, 3348, 3607, 3785, 3901, 3975, 4022, 4050, 4068, 4079, 4085,
                                  4089, 4092, 4093, 4094};
        if (d > 2047) return 4095;
        if (d < -2047) return 0;
        int w = d & 127, i = (d >> 7) + 16;
        return (T[i] * (128 - w) + T[i + 1] * w + 64) >> 7;
    }
    Tabs() {
        for (int d = -2048; d < 2048; ++d) sq[d + 2048] = squash(d);
        int pi = 0;
        for (int x = -2047; x < 2048; ++x) {
            int v = squash(x);
            for (int i = pi; i <= v; ++i) st[i] = x;
            pi = v + 1;
        }
        for (int i = pi; i < 4096; ++i) st[i] = 2047;
    }
};
const Tabs TB;

inline int qrank(int r) { return r < 4 ? r : r < 6 ? 4 : r < 10 ? 5 : r < 20 ? 6 : 7; }
inline int bitlen32(uint32_t v) { int b = 0; while (v) { ++b; v >>= 1; } return b; }
inline int qzero(uint32_t z) { return z < 8 ? int(z) : std::min(15, 5 + bitlen32(z)); }

constexpr int NODES = 32;
constexpr int NIN = 5;                // 4 contexts + bias

// Rank coder: one binary decision tree per rank (zero flag, Elias-gamma length in unary, two modelled mantissa
// bits, raw rest), each decision predicted by four adaptive contexts mixed in the logistic domain, then an APM.
template <bool ENC>
Bytes run(size_t n, const uint8_t* in, const uint8_t* pay, size_t plen) {
    if (n == 0) return {};
    std::vector<uint16_t> t0(size_t(NODES) * 64, 32768), t1(size_t(NODES) * 256, 32768),
        t2(size_t(NODES) << 16, 32768), t3(size_t(NODES) * 128, 32768);
    std::vector<int32_t> W(size_t(NODES) * 8 * NIN, 13000);
    std::vector<uint16_t> A(size_t(NODES) * 8 * 33);
    for (size_t i = 0; i < size_t(NODES) * 8; ++i)
        for (int j = 0; j < 33; ++j) A[i * 33 + j] = uint16_t(std::min(65535, TB.squash((j - 16) * 128) * 16));
    for (size_t i = 0; i < W.size(); i += NIN) W[i + 4] = 0;
    uint8_t mtf[256];
    for (int i = 0; i < 256; ++i) mtf[i] = uint8_t(i);
    Bytes out;
    Bytes dec;
    if (ENC) out.reserve(n / 3 + 16); else dec.resize(n);
    uint32_t x1 = 0, x2 = 0xFFFFFFFFu, x = 0;
    size_t pos = 0;
    if (!ENC) {
        for (int i = 0; i < 4; ++i) x = (x << 8) | (plen > size_t(i) ? pay[i] : 0);
        pos = 4;
    }
    int r1 = 0, r2 = 0, q1 = 0, q2 = 0;
    uint32_t c1 = 0, c2 = 0, zrun = 0;

    auto code = [&](int node, int bit) -> int {
        uint16_t* p0 = &t0[(size_t(q1) * 8 + size_t(q2)) * NODES + size_t(node)];
        uint16_t* p1 = &t1[size_t(c1) * NODES + size_t(node)];
        uint16_t* p2 = &t2[(((size_t(c1) << 8) | c2) << 5) | size_t(node)];
        uint16_t* p3 = &t3[(size_t(qzero(zrun)) * 8 + size_t(q1)) * NODES + size_t(node)];
        const int x0 = TB.st[*p0 >> 4], xa = TB.st[*p1 >> 4], xb = TB.st[*p2 >> 4], xc = TB.st[*p3 >> 4];
        int32_t* w = &W[(size_t(q1) * NODES + size_t(node)) * NIN];
        int64_t dot = int64_t(w[0]) * x0 + int64_t(w[1]) * xa + int64_t(w[2]) * xb + int64_t(w[3]) * xc + int64_t(w[4]) * 256;
        int d = int(dot >> 16);
        d = d > 2047 ? 2047 : d < -2047 ? -2047 : d;
        const int pm = TB.sq[d + 2048];
        const int av = d + 2048, aw = av & 127;
        uint16_t* ap = &A[(size_t(q1) * NODES + size_t(node)) * 33 + size_t(av >> 7)];
        const int pa = (ap[0] * (128 - aw) + ap[1] * aw) >> 7;
        int p = (pm * 16 + 3 * pa + 2) >> 2;
        p = p < 8 ? 8 : p > 65528 ? 65528 : p;
        const uint32_t xmid = x1 + uint32_t((uint64_t(x2 - x1) * uint64_t(p)) >> 16);
        if (!ENC) bit = x <= xmid ? 1 : 0;
        if (bit) x2 = xmid; else x1 = xmid + 1;
        while (((x1 ^ x2) & 0xFF000000u) == 0) {
            if (ENC) out.push_back(uint8_t(x2 >> 24));
            else { x = (x << 8) | (pos < plen ? pay[pos] : 0); ++pos; }
            x1 <<= 8;
            x2 = (x2 << 8) | 255u;
        }
        const int err = ((bit << 12) - pm) * 10;
        w[0] += (x0 * err) >> 14; w[1] += (xa * err) >> 14; w[2] += (xb * err) >> 14;
        w[3] += (xc * err) >> 14; w[4] += (256 * err) >> 14;
        const int tgt = bit ? 65535 : 0;
        *p0 = uint16_t(*p0 + ((tgt - int(*p0)) >> 6));
        *p1 = uint16_t(*p1 + ((tgt - int(*p1)) >> 5));
        *p2 = uint16_t(*p2 + ((tgt - int(*p2)) >> 4));
        *p3 = uint16_t(*p3 + ((tgt - int(*p3)) >> 6));
        const int rnd = bit ? 63 : 0;
        ap[0] = uint16_t(ap[0] + ((tgt - int(ap[0]) + rnd) >> 6));
        ap[1] = uint16_t(ap[1] + ((tgt - int(ap[1]) + rnd) >> 6));
        return bit;
    };
    auto raw_bit = [&](int bit) -> int {
        const uint32_t xmid = x1 + ((x2 - x1) >> 1);
        if (!ENC) bit = x <= xmid ? 1 : 0;
        if (bit) x2 = xmid; else x1 = xmid + 1;
        while (((x1 ^ x2) & 0xFF000000u) == 0) {
            if (ENC) out.push_back(uint8_t(x2 >> 24));
            else { x = (x << 8) | (pos < plen ? pay[pos] : 0); ++pos; }
            x1 <<= 8;
            x2 = (x2 << 8) | 255u;
        }
        return bit;
    };

    for (size_t i = 0; i < n; ++i) {
        int r = 0;
        uint8_t sym = 0;
        if (ENC) {
            sym = in[i];
            r = int(static_cast<const uint8_t*>(std::memchr(mtf, sym, 256)) - mtf);
        }
        // zero flag
        const int z = code(0, ENC ? (r == 0) : 0);
        if (z) {
            r = 0;
        } else {
            const uint32_t g0 = ENC ? uint32_t(r) : 0;                 // g >= 1
            const int nb0 = ENC ? bitlen32(g0) - 1 : 0;
            int nb = 0;
            while (nb < 7) {
                const int more = code(1 + nb, ENC ? (nb0 > nb) : 0);
                if (!more) break;
                ++nb;
            }
            uint32_t g = 1;
            for (int k = nb - 1; k >= 0; --k) {
                const int b0 = ENC ? int((g0 >> k) & 1) : 0;
                int b;
                const int depth = nb - 1 - k;                           // 0 = top mantissa bit
                if (depth == 0) b = code(8 + nb, b0);
                else if (depth == 1) b = code(16 + nb * 2 + int(g & 1), b0);
                else b = raw_bit(b0);
                g = (g << 1) | uint32_t(b);
            }
            r = int(g);
        }
        if (!ENC) {
            sym = mtf[r];
            dec[i] = sym;
        }
        if (r) std::memmove(mtf + 1, mtf, size_t(r));
        mtf[0] = sym;
        r2 = r1; r1 = r; q2 = q1; q1 = qrank(r);
        zrun = r == 0 ? zrun + 1 : 0;
        c2 = c1; c1 = sym;
    }
    (void)r2;
    if (ENC) {
        for (int sft = 24; sft >= 0; sft -= 8) out.push_back(uint8_t(x1 >> sft));
        return out;
    }
    return dec;
}

// ---------------------------------------------------------------- container
void put_varint(Bytes& o, uint64_t v) {
    while (v >= 128) { o.push_back(uint8_t(v | 128)); v >>= 7; }
    o.push_back(uint8_t(v));
}
uint64_t get_varint(const uint8_t* b, size_t end, size_t& pos) {
    uint64_t v = 0;
    for (int sh = 0; sh < 64; sh += 7) {
        if (pos >= end) throw FormatError("truncated");
        uint8_t c = b[pos++];
        v |= uint64_t(c & 127) << sh;
        if (!(c & 128)) return v;
    }
    throw FormatError("bad varint");
}
uint64_t chk64(const uint8_t* p, size_t n) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
    }
    uint64_t t = 0;
    for (size_t k = 0; i + k < n; ++k) t |= uint64_t(p[i + k]) << (8 * k);
    h = (h ^ t) * 0xC4CEB9FE1A85EC53ull;
    return h ^ (h >> 29);
}

Bytes encode_block(const uint8_t* d, size_t n) {
    Bytes rec;
    const uint64_t ck = chk64(d, n);
    auto raw = [&]() {
        Bytes r;
        r.push_back(0);
        put_varint(r, n);
        for (int i = 0; i < 8; ++i) r.push_back(uint8_t(ck >> (8 * i)));
        r.insert(r.end(), d, d + n);
        return r;
    };
    if (n < 16) return raw();
    Bytes L(n);
    bwt::Header h;
    bwt::forward(d, n, L.data(), h);
    Bytes pay = rank_encode(L.data(), n);
    rec.push_back(1);
    put_varint(rec, n);
    for (int i = 0; i < 8; ++i) rec.push_back(uint8_t(ck >> (8 * i)));
    put_varint(rec, h.primary);
    for (int j = 0; j < bwt::kChains - 1; ++j) put_varint(rec, h.starts[j]);
    put_varint(rec, pay.size());
    rec.insert(rec.end(), pay.begin(), pay.end());
    return rec.size() < n + 12 ? rec : raw();
}

unsigned pick_threads(unsigned t, size_t jobs) {
    if (t == 0) t = std::max(1u, std::thread::hardware_concurrency());
    return unsigned(std::min<size_t>(t, std::max<size_t>(jobs, 1)));
}

template <class F>
void parallel_for(size_t jobs, unsigned threads, F f) {
    threads = pick_threads(threads, jobs);
    if (threads <= 1) { for (size_t i = 0; i < jobs; ++i) f(i); return; }
    std::atomic<size_t> next{0};
    std::vector<std::thread> ts;
    for (unsigned k = 0; k < threads; ++k)
        ts.emplace_back([&] { for (size_t i; (i = next++) < jobs;) f(i); });
    for (auto& t : ts) t.join();
}

}  // namespace

Bytes rank_encode(const uint8_t* L, size_t n) { return run<true>(n, L, nullptr, 0); }
Bytes rank_decode(const uint8_t* payload, size_t plen, size_t n) { return run<false>(n, nullptr, payload, plen); }

Bytes compress(const Bytes& in, size_t block_size, unsigned threads) {
    if (block_size < 16 || block_size > bwt::kMaxBlock) throw std::invalid_argument("block_size out of range");
    const size_t n = in.size();
    const size_t nb = (n + block_size - 1) / block_size;
    std::vector<Bytes> recs(nb);
    parallel_for(nb, threads, [&](size_t i) {
        const size_t off = i * block_size;
        recs[i] = encode_block(in.data() + off, std::min(block_size, n - off));
    });
    Bytes out = {'A', 'M', 'D', 'B', VERSION};
    put_varint(out, n);
    put_varint(out, block_size);
    for (auto& r : recs) out.insert(out.end(), r.begin(), r.end());
    return out;
}

Bytes decompress(const Bytes& in, uint64_t max_output, unsigned threads) {
    const uint8_t* b = in.data();
    const size_t end = in.size();
    if (end < 5 || std::memcmp(b, "AMDB", 4) != 0) throw FormatError("bad magic");
    if (b[4] != VERSION) throw FormatError("unsupported version");
    size_t pos = 5;
    const uint64_t total = get_varint(b, end, pos);
    const uint64_t bs = get_varint(b, end, pos);
    if (total > max_output) throw FormatError("output exceeds limit");
    if (bs < 16 || bs > bwt::kMaxBlock) throw FormatError("bad block size");
    if (total / MAX_RATIO > end) throw FormatError("implausible expansion ratio");
    struct Rec { int type; size_t n; uint64_t ck; bwt::Header h; size_t pay, plen; };
    std::vector<Rec> recs;
    uint64_t got = 0;
    while (got < total) {
        Rec r{};
        if (pos >= end) throw FormatError("truncated");
        r.type = b[pos++];
        r.n = size_t(get_varint(b, end, pos));
        if (r.n == 0 || r.n > bs || r.n != std::min<uint64_t>(bs, total - got)) throw FormatError("bad block length");
        if (end - pos < 8) throw FormatError("truncated");
        r.ck = 0;
        for (int i = 0; i < 8; ++i) r.ck |= uint64_t(b[pos + i]) << (8 * i);
        pos += 8;
        if (r.type == 0) {
            if (end - pos < r.n) throw FormatError("truncated");
            r.pay = pos; r.plen = r.n; pos += r.n;
        } else if (r.type == 1) {
            r.h.primary = uint32_t(get_varint(b, end, pos));
            for (int j = 0; j < bwt::kChains - 1; ++j) r.h.starts[j] = uint32_t(get_varint(b, end, pos));
            r.plen = size_t(get_varint(b, end, pos));
            if (end - pos < r.plen) throw FormatError("truncated");
            r.pay = pos; pos += r.plen;
            if (r.plen / MAX_RATIO > r.n) throw FormatError("implausible block");
        } else {
            throw FormatError("unknown block type");
        }
        recs.push_back(r);
        got += r.n;
    }
    if (pos != end) throw FormatError("trailing data");
    Bytes out(static_cast<size_t>(total));
    std::atomic<bool> bad{false};
    parallel_for(recs.size(), threads, [&](size_t i) {
        const Rec& r = recs[i];
        uint8_t* dst = out.data() + i * size_t(bs);
        bool ok = true;
        if (r.type == 0) {
            std::memcpy(dst, b + r.pay, r.n);
        } else {
            Bytes L = rank_decode(b + r.pay, r.plen, r.n);
            ok = bwt::inverse(L.data(), r.n, r.h, dst);
        }
        if (!ok || chk64(dst, r.n) != r.ck) bad = true;
    });
    if (bad) throw FormatError("corrupt block (checksum mismatch)");
    return out;
}

}  // namespace bwtc
}  // namespace amdc1
