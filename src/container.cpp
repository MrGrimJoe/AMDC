// AMDC-1 container (spec v1.2 section 7). Format-identical to reference/amdc1/container.py.
#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>

#include "amdc1.hpp"
#include "transforms.hpp"

namespace amdc1 {
namespace {

constexpr uint8_t MAGIC[4] = {'A', 'M', 'D', 'C'};
constexpr uint8_t VERSION = 3;
constexpr uint64_t MAX_BLOCK = uint64_t(1) << 28;
constexpr uint64_t MAX_STRIDE = uint64_t(1) << 24;
constexpr uint64_t MAX_RATIO = 16384;
uint8_t cfg_byte(const CmCfg& c) { return uint8_t(c.lite | ((c.membits - kMinMemBits) << 1)); }
constexpr size_t SAMPLE = 12288;

struct Cand { int xf; uint32_t k; uint32_t s; };
const Cand CANDS[] = {{0, 0, 0}, {0, 0, 2}, {0, 0, 4}, {1, 1, 1}, {1, 2, 2},
                      {1, 4, 4}, {2, 1, 1}, {2, 4, 4}, {3, 2, 0}, {3, 4, 0}, {5, 0, 0}};
constexpr int NCANDS = int(sizeof(CANDS) / sizeof(CANDS[0]));

// Record/row length detection (encoder-side heuristic only; the chosen stride is stored in the stream).
// Returns up to two strides > 4: the best byte-equality lag and the best small-difference lag.
std::vector<uint32_t> detect_strides(const uint8_t* d, size_t n) {
    constexpr size_t W = 32768, SMAX = 4096;
    std::vector<uint32_t> out;
    if (n < 4 * 64) return out;
    size_t len = std::min(n, W + SMAX);
    const uint8_t* p = d + (n - len) / 2;
    size_t smax = std::min(SMAX, len / 8);
    uint64_t best_eq = 0, best_sad = ~uint64_t(0);
    uint32_t s_eq = 0, s_sad = 0;
    uint64_t sum_eq = 0;
    for (size_t s = 5; s <= smax; ++s) {
        uint64_t eq = 0, sad = 0;
        for (size_t i = smax; i < len; ++i) {
            int df = int(p[i]) - int(p[i - s]);
            eq += df == 0;
            sad += uint64_t(df < 0 ? -df : df);
        }
        sum_eq += eq;
        if (eq > best_eq) { best_eq = eq; s_eq = uint32_t(s); }
        if (sad < best_sad) { best_sad = sad; s_sad = uint32_t(s); }
    }
    const uint64_t cnt = smax >= 5 ? smax - 4 : 1, mean_eq = sum_eq / cnt;
    if (s_eq && best_eq > mean_eq * 3 / 2 + 64) out.push_back(s_eq);
    if (s_sad && s_sad != s_eq) out.push_back(s_sad);
    return out;
}

void put_varint(Bytes& o, uint64_t v) {
    while (v >= 0x80) { o.push_back(uint8_t(v | 0x80)); v >>= 7; }
    o.push_back(uint8_t(v));
}

uint64_t get_varint(const uint8_t* b, size_t end, size_t& pos) {
    uint64_t v = 0;
    int shift = 0;
    for (;;) {
        if (pos >= end || shift > 56) throw FormatError("bad varint");
        uint8_t c = b[pos++];
        v |= uint64_t(c & 0x7F) << shift;
        if (!(c & 0x80)) return v;
        shift += 7;
    }
}

Bytes raw_record(const uint8_t* d, size_t n) {
    Bytes r{0};
    put_varint(r, n);
    r.insert(r.end(), d, d + n);
    return r;
}

Bytes cm_record(size_t raw_len, int xf, const std::vector<uint64_t>& params, uint32_t s, const CmCfg& cfg, const Bytes& payload) {
    Bytes r{1};
    put_varint(r, raw_len);
    r.push_back(uint8_t(xf));
    for (uint64_t p : params) put_varint(r, p);
    put_varint(r, s);
    r.push_back(cfg_byte(cfg));
    put_varint(r, payload.size());
    r.insert(r.end(), payload.begin(), payload.end());
    return r;
}

std::vector<uint64_t> xf_params(const Cand& c) {
    return (c.xf >= 1 && c.xf <= 3) ? std::vector<uint64_t>{c.k} : std::vector<uint64_t>{};
}

// Encoder-side heuristic for level 0 (no search): x86 machine code has E8/E9 opcodes followed by a rel32 whose top
// byte is 00/FF at a rate of roughly 0.3-1% of positions; random or text data stays below 0.02%.
bool looks_like_x86(const uint8_t* d, size_t n) {
    size_t len = std::min<size_t>(n, 1 << 16);
    const uint8_t* p = d + (n - len) / 2;
    size_t hits = 0;
    for (size_t i = 0; i + 4 < len; ++i)
        if ((p[i] == 0xE8 || p[i] == 0xE9) && (p[i + 4] == 0x00 || p[i + 4] == 0xFF)) ++hits;
    return len > 4096 && hits * 1000 >= len * 2;   // >= 0.2%
}

Bytes make_sample(const uint8_t* d, size_t n) {
    if (n <= SAMPLE) return Bytes(d, d + n);
    size_t c = SAMPLE / 3, mid = (n - c) / 2;
    Bytes s(d, d + c);
    s.insert(s.end(), d + mid, d + mid + c);
    s.insert(s.end(), d + n - c, d + n);
    return s;
}

Bytes encode_cand(const uint8_t* d, size_t n, const Cand& c, const CmCfg& cfg) {
    Bytes t = xf_apply(c.xf, c.k, d, n);
    return cm_record(n, c.xf, xf_params(c), c.s, cfg, cm_encode(t.data(), n, c.s, cfg));
}

Bytes encode_block(const uint8_t* d, size_t n, int level, int membits) {
    const CmCfg cfg{uint8_t(level == 0 ? 1 : 0), uint8_t(membits)};
    Bytes raw = raw_record(d, n);
    if (n == 0) return raw;
    std::vector<Cand> cands;
    if (level == 0) {
        cands.push_back(looks_like_x86(d, n) ? Cand{5, 0, 0} : CANDS[0]);
    } else {
        cands.assign(CANDS, CANDS + NCANDS);
        if (n > SAMPLE)
            for (uint32_t st : detect_strides(d, n)) cands.push_back({0, 0, st});
    }
    const int nc = int(cands.size());
    Bytes best;
    if (level >= 2 || n <= SAMPLE) {
        for (int i = 0; i < nc; ++i) {
            Bytes rec = encode_cand(d, n, cands[i], cfg);
            if (best.empty() || rec.size() < best.size()) best = std::move(rec);
        }
    } else {
        Bytes smp = make_sample(d, n);
        size_t bs = 0;
        int bi = -1;
        for (int i = 0; i < nc; ++i) {
            Bytes t = xf_apply(cands[i].xf, cands[i].k, smp.data(), smp.size());
            size_t sc = cm_encode(t.data(), t.size(), cands[i].s, cfg).size();
            if (bi < 0 || sc < bs) { bs = sc; bi = i; }
        }
        if (bs >= smp.size()) return raw;
        best = encode_cand(d, n, cands[bi], cfg);
    }
    return best.size() < raw.size() ? best : raw;
}

bool image_record(const uint8_t* d, size_t n, int level, int membits, Bytes& rec) {
    const CmCfg cfg{uint8_t(level == 0 ? 1 : 0), uint8_t(membits)};
    PnmInfo pi;
    if (!parse_pnm(d, n, pi)) return false;
    uint32_t s = uint32_t(std::min<uint64_t>(pi.w, MAX_STRIDE));
    std::vector<int> cts;
    if (pi.ch == 3) { cts.push_back(1); if (level >= 1) cts.push_back(0); }
    else cts.push_back(0);
    bool have = false;
    for (int ct : cts) {
        Bytes y = image_fwd(d, n, pi, ct);
        Bytes r = cm_record(n, XF_IMAGE, {pi.hdr, pi.w, pi.h, pi.ch, uint64_t(ct)}, s, cfg,
                            cm_encode(y.data(), n, s, cfg));
        if (!have || r.size() < rec.size()) { rec = std::move(r); have = true; }
    }
    return have;
}

unsigned pick_threads(unsigned t, size_t jobs) {
    if (!t) t = std::max(1u, std::thread::hardware_concurrency());
    return unsigned(std::max<size_t>(1, std::min<size_t>(t, jobs)));
}

template <class F>
void parallel_for(size_t jobs, unsigned threads, F f) {
    threads = pick_threads(threads, jobs);
    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};
    auto worker = [&] {
        try {
            for (size_t i; !failed && (i = next++) < jobs;) f(i);
        } catch (...) { failed = true; }
    };
    std::vector<std::thread> pool;
    for (unsigned i = 1; i < threads; ++i) pool.emplace_back(worker);
    worker();
    for (auto& th : pool) th.join();
    if (failed) throw std::runtime_error("worker failed");
}

struct Rec {
    uint8_t method = 0;
    uint64_t n = 0, off = 0;
    int xf = 0;
    uint64_t params[5] = {0, 0, 0, 0, 0};
    uint64_t s = 0;
    CmCfg cfg;
    size_t pay = 0, plen = 0;
};

}  // namespace

uint32_t crc32(const uint8_t* p, size_t n) {
    static const auto tab = [] {
        std::vector<uint32_t> t(256);
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = tab[(c ^ p[i]) & 255] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

Bytes compress(const Bytes& in, size_t block_size, int level, unsigned threads, int membits) {
    if (block_size < 1 || block_size > MAX_BLOCK) throw std::invalid_argument("block_size out of range");
    if (membits < kMinMemBits || membits > kMaxMemBits) throw std::invalid_argument("membits out of range");
    const size_t n = in.size();
    const uint32_t crc = crc32(in.data(), n);
    auto wrap = [&](uint64_t bs, const std::vector<Bytes>& recs) {
        Bytes o(MAGIC, MAGIC + 4);
        o.push_back(VERSION);
        put_varint(o, n);
        put_varint(o, bs);
        for (const Bytes& r : recs) o.insert(o.end(), r.begin(), r.end());
        for (int i = 0; i < 4; ++i) o.push_back(uint8_t(crc >> (8 * i)));
        return o;
    };
    size_t nb = (n + block_size - 1) / block_size;
    std::vector<Bytes> recs(nb);
    parallel_for(nb, threads, [&](size_t i) {
        size_t off = i * block_size;
        recs[i] = encode_block(in.data() + off, std::min(block_size, n - off), level, membits);
    });
    Bytes best = wrap(block_size, recs);
    if (n <= MAX_BLOCK) {
        Bytes rec;
        if (image_record(in.data(), n, level, membits, rec)) {
            Bytes raw = raw_record(in.data(), n);
            Bytes cand = wrap(std::max<size_t>(n, 1), {rec.size() < raw.size() ? rec : raw});
            if (cand.size() < best.size()) best = std::move(cand);
        }
    }
    return best;
}

Bytes decompress(const Bytes& in, uint64_t max_output, unsigned threads) {
    const uint8_t* b = in.data();
    if (in.size() < 11 || std::memcmp(b, MAGIC, 4) != 0) throw FormatError("not an AMDC archive");
    if (b[4] != VERSION) throw FormatError("unsupported version");
    const size_t end = in.size() - 4;
    size_t pos = 5;
    uint64_t total = get_varint(b, end, pos);
    uint64_t bsize = get_varint(b, end, pos);
    if (total > max_output) throw FormatError("declared size exceeds limit");
    if (total && (bsize < 1 || bsize > MAX_BLOCK)) throw FormatError("bad block size");

    // Pass 1: parse and validate every record before allocating or decoding anything.
    std::vector<Rec> recs;
    uint64_t done = 0;
    while (done < total) {
        if (pos >= end) throw FormatError("truncated");
        Rec r;
        r.method = b[pos++];
        r.n = get_varint(b, end, pos);
        if (r.n < 1 || r.n > bsize || done + r.n > total) throw FormatError("bad block length");
        r.off = done;
        if (r.method == 0) {
            if (pos + r.n > end) throw FormatError("truncated raw block");
            r.pay = pos;
            r.plen = r.n;
            pos += r.n;
        } else if (r.method == 1) {
            if (pos >= end) throw FormatError("truncated");
            r.xf = b[pos++];
            if (r.xf > 5) throw FormatError("unknown transform");
            int cnt = (r.xf >= 1 && r.xf <= 3) ? 1 : r.xf == 4 ? 5 : 0;
            for (int i = 0; i < cnt; ++i) r.params[i] = get_varint(b, end, pos);
            r.s = get_varint(b, end, pos);
            if (pos >= end) throw FormatError("truncated");
            const uint8_t cb = b[pos++];
            r.cfg.lite = cb & 1;
            r.cfg.membits = uint8_t(kMinMemBits + (cb >> 1));
            if ((cb >> 1) > kMaxMemBits - kMinMemBits) throw FormatError("bad model configuration");
            uint64_t plen = get_varint(b, end, pos);
            if (r.s > MAX_STRIDE || plen > end - pos) throw FormatError("bad block header");
            if (r.n > (plen + 8) * MAX_RATIO) throw FormatError("implausible expansion ratio");
            if ((r.xf == 1 || r.xf == 2) && !(r.params[0] >= 1 && r.params[0] <= 255)) throw FormatError("bad stride");
            if (r.xf == 3 && !(r.params[0] >= 2 && r.params[0] <= 255)) throw FormatError("bad width");
            if (r.xf == 4) {
                uint64_t hdr = r.params[0], w = r.params[1], h = r.params[2], ch = r.params[3], ct = r.params[4];
                if ((ch != 1 && ch != 3) || ct > 1 || (ct && ch != 3) || w < 1 || h < 1 || w > r.n ||
                    h > r.n || hdr > r.n || w * h * ch > r.n - hdr)
                    throw FormatError("bad image parameters");
            }
            r.pay = pos;
            r.plen = size_t(plen);
            pos += r.plen;
        } else {
            throw FormatError("unknown block method");
        }
        done += r.n;
        recs.push_back(r);
    }
    if (pos != end) throw FormatError("trailing data");

    Bytes out(total);
    parallel_for(recs.size(), threads, [&](size_t i) {
        const Rec& r = recs[i];
        uint8_t* dst = out.data() + r.off;
        if (r.method == 0) {
            std::memcpy(dst, b + r.pay, r.n);
            return;
        }
        Bytes y = cm_decode(b + r.pay, r.plen, size_t(r.n), uint32_t(r.s), r.cfg);
        Bytes x;
        if (r.xf == XF_IMAGE) {
            PnmInfo pi{size_t(r.params[0]), uint32_t(r.params[1]), uint32_t(r.params[2]), uint32_t(r.params[3])};
            x = image_inv(y.data(), y.size(), pi, int(r.params[4]));
        } else {
            x = xf_invert(r.xf, uint32_t(r.params[0]), y.data(), y.size());
        }
        std::memcpy(dst, x.data(), x.size());
    });
    uint32_t want = 0;
    for (int i = 3; i >= 0; --i) want = (want << 8) | b[end + i];
    if (crc32(out.data(), out.size()) != want) throw FormatError("checksum mismatch");
    return out;
}

}  // namespace amdc1
