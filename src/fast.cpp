#include "fast.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace amdc1 {
namespace fast {
namespace {

inline uint32_t rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline uint64_t rd64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
inline unsigned rd16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
inline void wr16(uint8_t* p, unsigned v) { uint16_t x = uint16_t(v); std::memcpy(p, &x, 2); }
inline void copy16(uint8_t* d, const uint8_t* s) { std::memcpy(d, s, 16); }
inline void copy8(uint8_t* d, const uint8_t* s) { std::memcpy(d, s, 8); }

inline unsigned ctz64(uint64_t x) {
#if defined(_MSC_VER)
    unsigned long i;
    _BitScanForward64(&i, x);
    return unsigned(i);
#else
    return unsigned(__builtin_ctzll(x));
#endif
}

constexpr size_t MAX_DIST = 65535;
constexpr size_t MIN_MATCH = 4;
constexpr size_t LAST_LITERALS = 5;   // LZ4 format: the last 5 bytes are always literals
constexpr size_t MF_LIMIT = 12;       // ... and the last match starts at least 12 bytes before the end

inline size_t count_match(const uint8_t* a, const uint8_t* b, const uint8_t* limit) {
    const uint8_t* start = b;
    while (b + 8 <= limit) {
        uint64_t x = rd64(a) ^ rd64(b);
        if (x) return size_t(b - start) + (ctz64(x) >> 3);
        a += 8;
        b += 8;
    }
    while (b < limit && *a == *b) { ++a; ++b; }
    return size_t(b - start);
}

inline void put_len(uint8_t*& op, size_t r) {
    while (r >= 255) { *op++ = 255; r -= 255; }
    *op++ = uint8_t(r);
}

inline void emit(uint8_t*& op, const uint8_t* lit, size_t litlen, size_t offset, size_t mlen) {
    uint8_t* token = op++;
    size_t ml = mlen - MIN_MATCH;
    if (litlen >= 15) put_len(op, litlen - 15);
    if (litlen) std::memcpy(op, lit, litlen);
    op += litlen;
    wr16(op, unsigned(offset));
    op += 2;
    if (ml >= 15) put_len(op, ml - 15);
    *token = uint8_t(((litlen >= 15 ? 15 : litlen) << 4) | (ml >= 15 ? 15 : ml));
}

inline size_t emit_last(uint8_t* op0, const uint8_t* lit, size_t litlen) {
    uint8_t* op = op0;
    *op++ = uint8_t((litlen >= 15 ? 15 : litlen) << 4);
    if (litlen >= 15) put_len(op, litlen - 15);
    if (litlen) std::memcpy(op, lit, litlen);
    op += litlen;
    return size_t(op - op0);
}

inline unsigned hash4(uint32_t v, int bits) { return (v * 2654435761u) >> (32 - bits); }

size_t compress_level1(const uint8_t* src, size_t n, uint8_t* dst) {
    uint8_t* op = dst;
    if (n < MF_LIMIT + 1) return emit_last(dst, src, n);
    int hlog = 8;
    while (hlog < 16 && (size_t(1) << (hlog + 1)) <= n) ++hlog;
    static thread_local uint32_t tbl[1 << 16];
    std::memset(tbl, 0, sizeof(uint32_t) << hlog);
    const uint8_t* ip = src;
    const uint8_t* anchor = src;
    const uint8_t* const mflimit = src + n - MF_LIMIT;
    const uint8_t* const matchlimit = src + n - LAST_LITERALS;
    tbl[hash4(rd32(ip), hlog)] = 0;
    ++ip;
    for (;;) {
        const uint8_t* ref;
        const uint8_t* fwd = ip;
        size_t step = 1;
        unsigned skip = 1u << 5;
        for (;;) {
            ip = fwd;
            if (ip >= mflimit) goto done;
            unsigned h = hash4(rd32(ip), hlog);
            ref = src + tbl[h];
            tbl[h] = uint32_t(ip - src);
            fwd = ip + step;
            step = skip++ >> 5;
            if (size_t(ip - ref) <= MAX_DIST && rd32(ref) == rd32(ip)) break;
        }
        while (ip > anchor && ref > src && ip[-1] == ref[-1]) { --ip; --ref; }
        size_t mlen = MIN_MATCH + count_match(ref + MIN_MATCH, ip + MIN_MATCH, matchlimit);
        emit(op, anchor, size_t(ip - anchor), size_t(ip - ref), mlen);
        ip += mlen;
        anchor = ip;
        if (ip >= mflimit) break;
        tbl[hash4(rd32(ip - 2), hlog)] = uint32_t(ip - 2 - src);
    }
done:
    op += emit_last(op, anchor, size_t(src + n - anchor));
    return size_t(op - dst);
}

size_t compress_level2(const uint8_t* src, size_t n, uint8_t* dst) {
    uint8_t* op = dst;
    if (n < MF_LIMIT + 1) return emit_last(dst, src, n);
    constexpr int HLOG = 16;
    constexpr int DEPTH = 48;
    static thread_local int32_t head[1 << HLOG];
    static thread_local int32_t chain[1 << 16];
    std::memset(head, 0xFF, sizeof(head));
    const uint8_t* const mflimit = src + n - MF_LIMIT;
    const uint8_t* const matchlimit = src + n - LAST_LITERALS;
    const uint8_t* ip = src;
    const uint8_t* anchor = src;
    size_t next_ins = 0;
    auto insert_upto = [&](size_t pos) {
        for (; next_ins < pos; ++next_ins) {
            unsigned h = hash4(rd32(src + next_ins), HLOG);
            chain[next_ins & 0xFFFF] = head[h];
            head[h] = int32_t(next_ins);
        }
    };
    auto find = [&](const uint8_t* p, size_t& off) -> size_t {
        size_t best = MIN_MATCH - 1;
        insert_upto(size_t(p - src));
        int32_t cand = head[hash4(rd32(p), HLOG)];
        for (int d = 0; d < DEPTH && cand >= 0; ++d) {
            size_t dist = size_t(p - src) - size_t(cand);
            if (dist > MAX_DIST) break;
            const uint8_t* c = src + cand;
            if (p + best < matchlimit && c[best] == p[best] && rd32(c) == rd32(p)) {
                size_t l = MIN_MATCH + count_match(c + MIN_MATCH, p + MIN_MATCH, matchlimit);
                if (l > best) { best = l; off = dist; }
            }
            cand = chain[size_t(cand) & 0xFFFF];
        }
        return best >= MIN_MATCH ? best : 0;
    };
    while (ip < mflimit) {
        size_t off = 0, len = find(ip, off);
        if (!len) { ++ip; continue; }
        // lazy matching: prefer a clearly longer match starting one byte later
        while (ip + 1 < mflimit) {
            size_t off2 = 0, len2 = find(ip + 1, off2);
            if (len2 > len + 1) { ++ip; len = len2; off = off2; } else break;
        }
        emit(op, anchor, size_t(ip - anchor), off, len);
        ip += len;
        anchor = ip;
    }
    op += emit_last(op, anchor, size_t(src + n - anchor));
    return size_t(op - dst);
}

}  // namespace

size_t block_compress(const uint8_t* src, size_t n, uint8_t* dst, int level) {
    if (n > 0xFFFFFFF0u) throw FastError("block too large");  // positions are 32-bit
    return level >= 2 ? compress_level2(src, n, dst) : compress_level1(src, n, dst);
}

bool block_decompress(const uint8_t* ip, size_t inlen, uint8_t* dst, size_t outlen) {
    const uint8_t* const iend = ip + inlen;
    uint8_t* op = dst;
    uint8_t* const oend = dst + outlen;
    for (;;) {
        if (ip >= iend) return false;
        const unsigned token = *ip++;
        size_t lit = token >> 4;
        if (lit == 15) {
            unsigned s;
            do {
                if (ip >= iend) return false;
                s = *ip++;
                lit += s;
                if (lit > outlen) return false;
            } while (s == 255);
        }
        if (lit > size_t(iend - ip) || lit > size_t(oend - op)) return false;
        if (size_t(oend - op) >= lit + 16 && size_t(iend - ip) >= lit + 16) {
            const uint8_t* s = ip;
            uint8_t* d = op;
            uint8_t* const e = op + lit;
            do { copy16(d, s); d += 16; s += 16; } while (d < e);
        } else if (lit) {
            std::memcpy(op, ip, lit);
        }
        ip += lit;
        op += lit;
        if (ip == iend) return op == oend;
        if (iend - ip < 2) return false;
        const size_t offset = rd16(ip);
        ip += 2;
        if (offset == 0 || offset > size_t(op - dst)) return false;
        size_t mlen = token & 15;
        if (mlen == 15) {
            unsigned s;
            do {
                if (ip >= iend) return false;
                s = *ip++;
                mlen += s;
                if (mlen > outlen) return false;
            } while (s == 255);
        }
        mlen += MIN_MATCH;
        if (mlen > size_t(oend - op)) return false;
        const uint8_t* m = op - offset;
        const size_t room = size_t(oend - op);
        if (offset >= 16) {
            if (room >= mlen + 16) {
                uint8_t* d = op;
                uint8_t* const e = op + mlen;
                do { copy16(d, m); d += 16; m += 16; } while (d < e);
            } else {
                for (size_t i = 0; i < mlen; ++i) op[i] = m[i];
            }
        } else if (offset == 1) {
            std::memset(op, m[0], mlen);
        } else if (room >= mlen + 16) {
            // short period: write 16 bytes by hand, then continue from a multiple of the period >= 16
            size_t pre = mlen < 16 ? mlen : 16;
            for (size_t i = 0; i < pre; ++i) op[i] = m[i];
            if (mlen > 16) {
                const size_t off2 = offset * ((16 + offset - 1) / offset);
                uint8_t* d = op + 16;
                const uint8_t* s = d - off2;
                uint8_t* const e = op + mlen;
                do { copy16(d, s); d += 16; s += 16; } while (d < e);
            }
        } else {
            for (size_t i = 0; i < mlen; ++i) op[i] = m[i];
        }
        op += mlen;
    }
}

// ---------------------------------------------------------------------------------------------------
// pages

namespace {
bool all_same(const uint8_t* p, size_t n) {
    if (n == 0) return true;
    uint64_t pat;
    std::memset(&pat, p[0], 8);
    size_t i = 0;
    for (; i + 8 <= n; i += 8)
        if (rd64(p + i) != pat) return false;
    for (; i < n; ++i)
        if (p[i] != p[0]) return false;
    return true;
}
}  // namespace

size_t page_compress(const uint8_t* page, size_t n, uint8_t* out, int level, unsigned max_pct) {
    if (n == 0) return 0;
    if (all_same(page, n)) {
        if (page[0] == 0) { out[0] = PAGE_ZERO; return 1; }
        out[0] = PAGE_FILL;
        out[1] = page[0];
        return 2;
    }
    size_t c = block_compress(page, n, out + 1, level);
    if (c * 100 > n * size_t(max_pct)) return 0;
    out[0] = PAGE_LZ;
    return c + 1;
}

bool page_decompress(const uint8_t* enc, size_t len, uint8_t* page, size_t n) {
    if (len == 0) return false;
    switch (enc[0]) {
        case PAGE_RAW:
            if (len != n + 1) return false;
            std::memcpy(page, enc + 1, n);
            return true;
        case PAGE_ZERO:
            if (len != 1) return false;
            std::memset(page, 0, n);
            return true;
        case PAGE_FILL:
            if (len != 2) return false;
            std::memset(page, enc[1], n);
            return true;
        case PAGE_LZ:
            return block_decompress(enc + 1, len - 1, page, n);
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------------------------------
// container
//   "AMDF" | version u8 = 1 | flags u8 (bit0: checksum) | orig_len | block_size | blocks | [hash64 LE]
//   block: 0 raw (block_len bytes) | 1 lz: varint clen, data | 2 fill: u8

uint64_t hash64(const uint8_t* p, size_t n) {
    // 4 independent multiply-xorshift lanes (32 bytes per step) so the multiplies pipeline; ~15 GB/s.
    constexpr uint64_t K1 = 0xFF51AFD7ED558CCDull, K2 = 0xC4CEB9FE1A85EC53ull, K0 = 0x9E3779B97F4A7C15ull;
    uint64_t h0 = K0 ^ n, h1 = K1 + n, h2 = K2 ^ (n << 1), h3 = K0 + 3 * n;
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        h0 = (h0 ^ rd64(p + i)) * K1;       h0 ^= h0 >> 32;
        h1 = (h1 ^ rd64(p + i + 8)) * K1;   h1 ^= h1 >> 32;
        h2 = (h2 ^ rd64(p + i + 16)) * K1;  h2 ^= h2 >> 32;
        h3 = (h3 ^ rd64(p + i + 24)) * K1;  h3 ^= h3 >> 32;
    }
    uint64_t h = h0 ^ (h1 << 13 | h1 >> 51) ^ (h2 << 29 | h2 >> 35) ^ (h3 << 43 | h3 >> 21);
    for (; i + 8 <= n; i += 8) { h = (h ^ rd64(p + i)) * K1; h ^= h >> 32; }
    uint64_t t = 0;
    for (size_t k = 0; i < n; ++i, k += 8) t |= uint64_t(p[i]) << k;
    h = (h ^ t) * K2;
    h ^= h >> 29;
    return h;
}

namespace {
void put_varint(Bytes& o, uint64_t v) {
    while (v >= 0x80) { o.push_back(uint8_t(v | 0x80)); v >>= 7; }
    o.push_back(uint8_t(v));
}
uint64_t get_varint(const uint8_t* b, size_t end, size_t& pos) {
    uint64_t v = 0;
    int sh = 0;
    for (;;) {
        if (pos >= end || sh > 56) throw FastError("bad varint");
        uint8_t c = b[pos++];
        v |= uint64_t(c & 0x7F) << sh;
        if (!(c & 0x80)) return v;
        sh += 7;
    }
}
unsigned pick(unsigned t, size_t jobs) {
    if (!t) t = std::max(1u, std::thread::hardware_concurrency());
    return unsigned(std::max<size_t>(1, std::min<size_t>(t, jobs)));
}
template <class F>
void parallel_for(size_t jobs, unsigned threads, F f) {
    threads = pick(threads, jobs);
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
    for (auto& t : pool) t.join();
    if (failed) throw FastError("worker failed");
}
constexpr uint8_t MAGIC[4] = {'A', 'M', 'D', 'F'};
}  // namespace

Bytes compress(const Bytes& in, size_t block_size, int level, unsigned threads, bool checksum) {
    if (block_size < 4096 || block_size > (size_t(1) << 28)) throw std::invalid_argument("block_size out of range");
    const size_t n = in.size();
    const size_t nb = (n + block_size - 1) / block_size;
    std::vector<Bytes> recs(nb);
    parallel_for(nb, threads, [&](size_t i) {
        const size_t off = i * block_size, len = std::min(block_size, n - off);
        const uint8_t* p = in.data() + off;
        Bytes& r = recs[i];
        if (all_same(p, len)) { r = {2, p[0]}; return; }
        Bytes tmp(block_bound(len));
        size_t c = block_compress(p, len, tmp.data(), level);
        if (c < len) {
            r.push_back(1);
            put_varint(r, c);
            r.insert(r.end(), tmp.begin(), tmp.begin() + long(c));
        } else {
            r.push_back(0);
            r.insert(r.end(), p, p + len);
        }
    });
    Bytes o(MAGIC, MAGIC + 4);
    o.push_back(1);
    o.push_back(checksum ? 1 : 0);
    put_varint(o, n);
    put_varint(o, block_size);
    for (const Bytes& r : recs) o.insert(o.end(), r.begin(), r.end());
    if (checksum) {
        uint64_t h = hash64(in.data(), n);
        for (int i = 0; i < 8; ++i) o.push_back(uint8_t(h >> (8 * i)));
    }
    return o;
}

namespace {
struct Rec { uint8_t type; uint64_t off, len, pay, plen; };
struct Parsed { uint64_t total = 0; bool chk = false; size_t end = 0; std::vector<Rec> recs; };

Parsed parse(const uint8_t* b, size_t inlen, uint64_t max_output) {
    if (inlen < 8 || std::memcmp(b, MAGIC, 4) != 0) throw FastError("not an AMDF archive");
    if (b[4] != 1) throw FastError("unsupported version");
    Parsed P;
    P.chk = b[5] & 1;
    if (b[5] & ~1) throw FastError("unknown flags");
    if (P.chk && inlen < 8 + 8) throw FastError("truncated");
    P.end = inlen - (P.chk ? 8 : 0);
    size_t pos = 6;
    P.total = get_varint(b, P.end, pos);
    const uint64_t bsize = get_varint(b, P.end, pos);
    if (P.total > max_output) throw FastError("declared size exceeds limit");
    if (P.total && (bsize < 4096 || bsize > (uint64_t(1) << 28))) throw FastError("bad block size");
    for (uint64_t done = 0; done < P.total;) {
        if (pos >= P.end) throw FastError("truncated");
        Rec r{b[pos++], done, std::min<uint64_t>(bsize, P.total - done), 0, 0};
        if (r.type == 0) {
            if (r.len > P.end - pos) throw FastError("truncated raw block");
            r.pay = pos; r.plen = r.len; pos += size_t(r.len);
        } else if (r.type == 1) {
            uint64_t c = get_varint(b, P.end, pos);
            if (c > P.end - pos || c < 1) throw FastError("bad block");
            if (r.len > (c + 16) * 255) throw FastError("implausible expansion ratio");
            r.pay = pos; r.plen = c; pos += size_t(c);
        } else if (r.type == 2) {
            if (pos >= P.end) throw FastError("truncated");
            r.pay = pos++; r.plen = 1;
        } else {
            throw FastError("unknown block type");
        }
        done += r.len;
        P.recs.push_back(r);
    }
    if (pos != P.end) throw FastError("trailing data");
    return P;
}

void decode_all(const uint8_t* b, const Parsed& P, uint8_t* out, unsigned threads) {
    parallel_for(P.recs.size(), threads, [&](size_t i) {
        const Rec& r = P.recs[i];
        uint8_t* d = out + r.off;
        if (r.type == 0) std::memcpy(d, b + r.pay, size_t(r.len));
        else if (r.type == 2) std::memset(d, b[r.pay], size_t(r.len));
        else if (!block_decompress(b + r.pay, size_t(r.plen), d, size_t(r.len))) throw FastError("corrupt block");
    });
    if (P.chk) {
        uint64_t want = 0;
        for (int i = 7; i >= 0; --i) want = (want << 8) | b[P.end + size_t(i)];
        if (hash64(out, size_t(P.total)) != want) throw FastError("checksum mismatch");
    }
}
}  // namespace

size_t decompressed_size(const uint8_t* in, size_t inlen) { return size_t(parse(in, inlen, uint64_t(-1)).total); }

size_t decompress_into(const uint8_t* in, size_t inlen, uint8_t* dst, size_t cap, unsigned threads) {
    Parsed P = parse(in, inlen, cap);   // total > cap is rejected before anything is written
    decode_all(in, P, dst, threads);
    return size_t(P.total);
}

Bytes decompress(const Bytes& in, uint64_t max_output, unsigned threads) {
    Parsed P = parse(in.data(), in.size(), max_output);
    Bytes out(size_t(P.total));
    decode_all(in.data(), P, out.data(), threads);
    return out;
}

}  // namespace fast
}  // namespace amdc1
