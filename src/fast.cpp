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

// Hash tables are tagged with a per-thread "epoch" base instead of being cleared for every call: an entry
// holds base + position, and anything below the current base is stale and is treated as empty. A 4 KiB page
// would otherwise pay for a 16-256 KiB memset on every call, which dominated the cost of RAM compression.
struct MatchTables {
    uint32_t tbl[1 << 16];
    int32_t head[1 << 16];
    int32_t chain[1 << 16];
    uint32_t base = 1;
};
thread_local MatchTables g_mt;

inline uint32_t epoch_begin(MatchTables& t, size_t n) {
    // keep base + n well inside 31 bits (head/chain are int32); on wrap, wipe the tables once
    if (uint64_t(t.base) + 2 * uint64_t(n) + 64 > 0x7FFFFFFFull) {
        std::memset(t.tbl, 0, sizeof(t.tbl));
        std::memset(t.head, 0, sizeof(t.head));
        t.base = 1;
    }
    uint32_t b = t.base;
    t.base += uint32_t(n) + 1;
    return b;
}

size_t compress_level1(const uint8_t* src, size_t n, uint8_t* dst) {
    uint8_t* op = dst;
    if (n < MF_LIMIT + 1) return emit_last(dst, src, n);
    int hlog = 8;
    while (hlog < 16 && (size_t(1) << (hlog + 1)) <= n) ++hlog;
    uint32_t* const tbl = g_mt.tbl;
    const uint32_t base = epoch_begin(g_mt, n);
    const uint8_t* ip = src;
    const uint8_t* anchor = src;
    const uint8_t* const mflimit = src + n - MF_LIMIT;
    const uint8_t* const matchlimit = src + n - LAST_LITERALS;
    tbl[hash4(rd32(ip), hlog)] = base;
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
            const uint32_t e = tbl[h];
            tbl[h] = base + uint32_t(ip - src);
            fwd = ip + step;
            step = skip++ >> 5;
            if (e < base) continue;                    // stale entry from an earlier call
            ref = src + (e - base);
            if (size_t(ip - ref) <= MAX_DIST && rd32(ref) == rd32(ip)) break;
        }
        while (ip > anchor && ref > src && ip[-1] == ref[-1]) { --ip; --ref; }
        size_t mlen = MIN_MATCH + count_match(ref + MIN_MATCH, ip + MIN_MATCH, matchlimit);
        emit(op, anchor, size_t(ip - anchor), size_t(ip - ref), mlen);
        ip += mlen;
        anchor = ip;
        if (ip >= mflimit) break;
        tbl[hash4(rd32(ip - 2), hlog)] = base + uint32_t(ip - 2 - src);
    }
done:
    op += emit_last(op, anchor, size_t(src + n - anchor));
    return size_t(op - dst);
}

size_t compress_level2(const uint8_t* src, size_t n, uint8_t* dst) {
    uint8_t* op = dst;
    if (n < MF_LIMIT + 1) return emit_last(dst, src, n);
    int hlog = 9;
    while (hlog < 16 && (size_t(1) << hlog) < n) ++hlog;     // about one bucket per input position
    constexpr int DEPTH = 48;
    int32_t* const head = g_mt.head;
    int32_t* const chain = g_mt.chain;
    const int32_t base = int32_t(epoch_begin(g_mt, n));
    const uint8_t* const mflimit = src + n - MF_LIMIT;
    const uint8_t* const matchlimit = src + n - LAST_LITERALS;
    const uint8_t* ip = src;
    const uint8_t* anchor = src;
    size_t next_ins = 0;
    auto insert_upto = [&](size_t pos) {
        for (; next_ins < pos; ++next_ins) {
            unsigned h = hash4(rd32(src + next_ins), hlog);
            chain[next_ins & 0xFFFF] = head[h];
            head[h] = base + int32_t(next_ins);
        }
    };
    auto find = [&](const uint8_t* p, size_t& off) -> size_t {
        size_t best = MIN_MATCH - 1;
        insert_upto(size_t(p - src));
        int32_t cand = head[hash4(rd32(p), hlog)];
        for (int d = 0; d < DEPTH && cand >= base; ++d) {
            const size_t cpos = size_t(cand - base);
            const size_t dist = size_t(p - src) - cpos;
            if (dist > MAX_DIST) break;
            const uint8_t* c = src + cpos;
            if (p + best < matchlimit && c[best] == p[best] && rd32(c) == rd32(p)) {
                size_t l = MIN_MATCH + count_match(c + MIN_MATCH, p + MIN_MATCH, matchlimit);
                if (l > best) { best = l; off = dist; }
            }
            cand = chain[cpos & 0xFFFF];
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
    if (n > (size_t(1) << 30)) throw FastError("block too large");  // positions are tagged into 31 bits
    return level >= 2 ? compress_level2(src, n, dst) : compress_level1(src, n, dst);
}

// ---------------------------------------------------------------------------------------------------
// page format v2 (inputs up to kPage2Max bytes): LZ4-style tokens, but offsets cost 1 byte when < 240 and
// 1 byte when they repeat the previous offset, otherwise 2. Typical memory pages are full of near and
// strided repeats, so this saves a byte on most matches while the decoder stays a plain byte-aligned loop.
namespace {
constexpr size_t P2_MAXOFF = 240 + 4095;
}  // namespace
namespace { template <bool V2, bool SEP> bool decode_impl(const uint8_t*, size_t, uint8_t*, size_t, const uint8_t*, const uint8_t*); }
namespace {

inline unsigned p2_offcost(size_t off, size_t last) { return (off == last || off < 240) ? 1u : 2u; }

inline void p2_emit(uint8_t*& op, const uint8_t* lit, size_t litlen, size_t off, size_t last, size_t mlen) {
    uint8_t* token = op++;
    const size_t ml = mlen - MIN_MATCH;
    if (litlen >= 15) put_len(op, litlen - 15);
    if (litlen) std::memcpy(op, lit, litlen);
    op += litlen;
    if (off == last) *op++ = 0;
    else if (off < 240) *op++ = uint8_t(off);
    else { const size_t v = off - 240; *op++ = uint8_t(0xF0 | (v >> 8)); *op++ = uint8_t(v & 255); }
    if (ml >= 15) put_len(op, ml - 15);
    *token = uint8_t(((litlen >= 15 ? 15 : litlen) << 4) | (ml >= 15 ? 15 : ml));
}

size_t page2_fast(const uint8_t* src, size_t n, uint8_t* dst) {
    uint8_t* op = dst;
    if (n < 16) return emit_last(dst, src, n);
    constexpr int HLOG = 12;
    uint32_t* const tbl = g_mt.tbl;
    const uint32_t base = epoch_begin(g_mt, n);
    const uint8_t* const mflimit = src + n - 8;
    const uint8_t* const matchlimit = src + n;
    const uint8_t* ip = src + 1;
    const uint8_t* anchor = src;
    size_t last = 0;
    tbl[hash4(rd32(src), HLOG)] = base;
    for (;;) {
        const uint8_t* ref = nullptr;
        size_t off = 0;
        const uint8_t* fwd = ip;
        size_t step = 1;
        unsigned skip = 1u << 5;
        for (;;) {
            ip = fwd;
            if (ip >= mflimit) goto done;
            const uint32_t v = rd32(ip);
            const unsigned h = hash4(v, HLOG);
            const uint32_t e = tbl[h];
            tbl[h] = base + uint32_t(ip - src);
            fwd = ip + step;
            step = skip++ >> 5;
            if (last && size_t(ip - src) >= last && rd32(ip - last) == v) { ref = ip - last; off = last; break; }
            if (e < base) continue;
            ref = src + (e - base);
            off = size_t(ip - ref);
            if (off <= P2_MAXOFF && rd32(ref) == v) break;
        }
        while (ip > anchor && ref > src && ip[-1] == ref[-1]) { --ip; --ref; }
        const size_t mlen = MIN_MATCH + count_match(ref + MIN_MATCH, ip + MIN_MATCH, matchlimit);
        p2_emit(op, anchor, size_t(ip - anchor), off, last, mlen);
        last = off;
        ip += mlen;
        anchor = ip;
        if (ip >= mflimit) break;
        tbl[hash4(rd32(ip - 2), HLOG)] = base + uint32_t(ip - 2 - src);
    }
done:
    op += emit_last(op, anchor, size_t(src + n - anchor));
    return size_t(op - dst);
}

size_t page2_chain(const uint8_t* src, size_t n, uint8_t* dst) {
    uint8_t* op = dst;
    if (n < 16) return emit_last(dst, src, n);
    constexpr int HLOG = 12, DEPTH = 24;
    int32_t* const head = g_mt.head;
    int32_t* const chain = g_mt.chain;
    const int32_t base = int32_t(epoch_begin(g_mt, n));
    const uint8_t* const mflimit = src + n - 8;
    const uint8_t* const matchlimit = src + n;
    const uint8_t* ip = src;
    const uint8_t* anchor = src;
    size_t last = 0, next_ins = 0;
    auto insert_upto = [&](size_t pos) {
        for (; next_ins < pos; ++next_ins) {
            const unsigned h = hash4(rd32(src + next_ins), HLOG);
            chain[next_ins] = head[h];
            head[h] = base + int32_t(next_ins);
        }
    };
    // value of a candidate: length is worth 2 per byte, every offset byte costs 1
    auto find = [&](const uint8_t* p, size_t& boff, size_t& blen) {
        blen = 0; boff = 0;
        long bestv = 0;
        const size_t pos = size_t(p - src);
        insert_upto(pos);
        if (last && pos >= last && rd32(p - last) == rd32(p)) {
            blen = MIN_MATCH + count_match(p - last + MIN_MATCH, p + MIN_MATCH, matchlimit);
            boff = last;
            bestv = long(blen) * 2 - 1;
        }
        int32_t cand = head[hash4(rd32(p), HLOG)];
        for (int d = 0; d < DEPTH && cand >= base; ++d) {
            const size_t cpos = size_t(cand - base);
            const size_t dist = pos - cpos;
            if (dist > P2_MAXOFF) break;
            const uint8_t* c = src + cpos;
            if (rd32(c) == rd32(p) && (blen < MIN_MATCH || p + blen < matchlimit) && (blen < MIN_MATCH || c[blen] == p[blen] || true)) {
                const size_t l = MIN_MATCH + count_match(c + MIN_MATCH, p + MIN_MATCH, matchlimit);
                const long v = long(l) * 2 - long(p2_offcost(dist, last));
                if (v > bestv) { bestv = v; blen = l; boff = dist; }
            }
            cand = chain[cpos];
        }
    };
    while (ip < mflimit) {
        size_t off, len;
        find(ip, off, len);
        if (!len) { ++ip; continue; }
        while (ip + 1 < mflimit) {
            size_t off2, len2;
            find(ip + 1, off2, len2);
            const long v1 = long(len) * 2 - long(p2_offcost(off, last));
            const long v2 = long(len2) * 2 - long(p2_offcost(off2, last)) - 2;   // one extra literal byte
            if (len2 && v2 > v1) { ++ip; len = len2; off = off2; } else break;
        }
        p2_emit(op, anchor, size_t(ip - anchor), off, last, len);
        last = off;
        ip += len;
        anchor = ip;
    }
    op += emit_last(op, anchor, size_t(src + n - anchor));
    return size_t(op - dst);
}
}  // namespace


// ---------------------------------------------------------------------------------------------------
// page format v3 = page v2 sequences + Huffman-coded literals (only used when it makes the page smaller).
//   PAGE_LZ2 : page2 stream
//   PAGE_HUF : code-length header (nibbles, zero runs) | nlit u16 | lenA u16 | lenB u16 | stream A | stream B | sequences
// Literals are split in two halves coded into two LSB-first bit streams that are decoded interleaved, so the
// two table-lookup dependency chains overlap. The code is a complete canonical Huffman code, max length 10.
namespace {
constexpr int HUF_MAXLEN = 10;

// code lengths for 256 byte frequencies; returns false if fewer than 2 symbols are used
bool huff_lengths(const uint32_t* freq_in, uint8_t* len) {
    uint32_t freq[256];
    int nsym = 0;
    for (int i = 0; i < 256; ++i) { freq[i] = freq_in[i]; nsym += freq[i] != 0; }
    if (nsym < 2) return false;
    for (;;) {
        struct Leaf { uint32_t f; int sym; } leaf[256];
        int nl = 0;
        for (int i = 0; i < 256; ++i) if (freq[i]) leaf[nl++] = {freq[i], i};
        std::sort(leaf, leaf + nl, [](const Leaf& a, const Leaf& b) { return a.f != b.f ? a.f < b.f : a.sym < b.sym; });
        // two-queue Huffman construction: nodes 0..nl-1 are leaves, nl.. internal
        uint32_t w[512];
        int parent[512];
        for (int i = 0; i < nl; ++i) w[i] = leaf[i].f;
        int li = 0, ni = nl, nn = nl;
        auto take = [&]() -> int {
            if (li < nl && (ni >= nn || w[li] <= w[ni])) return li++;
            return ni++;
        };
        for (int k = 0; k < nl - 1; ++k) {
            int a = take(), b = take();
            w[nn] = w[a] + w[b];
            parent[a] = parent[b] = nn;
            ++nn;
        }
        int depth[512];
        depth[nn - 1] = 0;
        int maxd = 0;
        for (int i = nn - 2; i >= 0; --i) { depth[i] = depth[parent[i]] + 1; if (i < nl) maxd = std::max(maxd, depth[i]); }
        if (maxd <= HUF_MAXLEN) {
            std::memset(len, 0, 256);
            for (int i = 0; i < nl; ++i) len[leaf[i].sym] = uint8_t(depth[i]);
            return true;
        }
        for (int i = 0; i < 256; ++i) if (freq[i]) freq[i] = (freq[i] + 1) >> 1;   // flatten and retry
    }
}

struct HufEnc { uint16_t code[256]; };   // bit-reversed, LSB-first

void huff_codes(const uint8_t* len, HufEnc& e) {
    unsigned count[HUF_MAXLEN + 2] = {0}, next[HUF_MAXLEN + 2];
    for (int i = 0; i < 256; ++i) count[len[i]]++;
    count[0] = 0;
    unsigned code = 0;
    for (int l = 1; l <= HUF_MAXLEN; ++l) { code = (code + count[l - 1]) << 1; next[l] = code; }
    for (int i = 0; i < 256; ++i) {
        const int l = len[i];
        if (!l) { e.code[i] = 0; continue; }
        unsigned c = next[l]++, r = 0;
        for (int b = 0; b < l; ++b) r |= ((c >> b) & 1u) << (l - 1 - b);
        e.code[i] = uint16_t(r);
    }
}

struct BitW {
    uint8_t* out; uint64_t acc = 0; int nb = 0;
    void put(unsigned code, int l) { acc |= uint64_t(code) << nb; nb += l; while (nb >= 8) { *out++ = uint8_t(acc); acc >>= 8; nb -= 8; } }
    void flush() { if (nb > 0) { *out++ = uint8_t(acc); acc = 0; nb = 0; } }
};

// builds the 2^tb decode table from code lengths; validates that the code is complete. Returns tb or 0.
int huff_table(const uint8_t* len, uint16_t* tab) {
    unsigned count[HUF_MAXLEN + 2] = {0};
    int maxl = 0;
    for (int i = 0; i < 256; ++i) { if (len[i] > HUF_MAXLEN) return 0; count[len[i]]++; maxl = std::max<int>(maxl, len[i]); }
    count[0] = 0;
    if (maxl == 0) return 0;
    uint32_t kraft = 0;
    for (int l = 1; l <= maxl; ++l) kraft += count[l] << (maxl - l);
    if (kraft != (1u << maxl)) return 0;
    unsigned next[HUF_MAXLEN + 2], code = 0;
    for (int l = 1; l <= HUF_MAXLEN; ++l) { code = (code + count[l - 1]) << 1; next[l] = code; }
    const unsigned size = 1u << maxl;
    for (int i = 0; i < 256; ++i) {
        const int l = len[i];
        if (!l) continue;
        unsigned c = next[l]++, r = 0;
        for (int b = 0; b < l; ++b) r |= ((c >> b) & 1u) << (l - 1 - b);
        const uint16_t ent = uint16_t(i | (l << 8));
        for (unsigned j = r; j < size; j += 1u << l) tab[j] = ent;
    }
    return maxl;
}

struct BitR {
    const uint8_t* p; const uint8_t* end; uint64_t acc = 0; int nb = 0; const uint8_t* start;
    inline void refill() {
        if (end - p >= 8) { uint64_t v; std::memcpy(&v, p, 8); acc |= v << nb; p += (63 - nb) >> 3; nb |= 56; }
        else { while (nb <= 56 && p < end) { acc |= uint64_t(*p++) << nb; nb += 8; } }
    }
    // bits consumed so far must not exceed what the stream holds
    bool ok() const { return nb >= 0 && (int64_t(p - start) * 8 - nb) <= int64_t(end - start) * 8; }
};
}  // namespace

size_t page3_huf(const uint8_t* lz2, size_t lzlen, uint8_t* out) {
    // split the page2 stream into literals + literal-free sequences
    uint8_t lits[kPage2Max + 32];
    uint8_t seq[kPage2Max * 2 + 64];
    size_t nlit = 0, ns = 0;
    const uint8_t* ip = lz2;
    const uint8_t* const e = lz2 + lzlen;
    for (;;) {
        const uint8_t t = *ip++;
        seq[ns++] = t;
        size_t lit = t >> 4;
        if (lit == 15) { uint8_t b; do { b = *ip++; seq[ns++] = b; lit += b; } while (b == 255); }
        std::memcpy(lits + nlit, ip, lit);
        nlit += lit; ip += lit;
        if (ip >= e) break;
        const uint8_t o = *ip++;
        seq[ns++] = o;
        if (o >= 0xF0) seq[ns++] = *ip++;
        if ((t & 15) == 15) { uint8_t b; do { b = *ip++; seq[ns++] = b; } while (b == 255); }
    }
    if (nlit < 96) return 0;
    uint32_t freq[256] = {0};
    for (size_t i = 0; i < nlit; ++i) freq[lits[i]]++;
    uint8_t len[256];
    if (!huff_lengths(freq, len)) return 0;
    // cheap bound: if the coded literals plus header cannot beat the raw literals, stop here
    HufEnc he;
    huff_codes(len, he);
    uint64_t bits = 0;
    for (int i = 0; i < 256; ++i) bits += uint64_t(freq[i]) * len[i];
    if ((bits + 7) / 8 + 70 >= nlit) return 0;

    uint8_t* op = out;
    // header: nibbles
    uint8_t nib[512];
    int nn = 0;
    for (int i = 0; i < 256;) {
        if (len[i]) { nib[nn++] = len[i]; ++i; continue; }
        int r = 0;
        while (i + r < 256 && !len[i + r] && r < 16) ++r;
        nib[nn++] = 0; nib[nn++] = uint8_t(r - 1);
        i += r;
    }
    for (int i = 0; i < nn; i += 2) *op++ = uint8_t(nib[i] | ((i + 1 < nn ? nib[i + 1] : 0) << 4));
    uint8_t* const hdr = op;
    op += 6;
    const size_t n1 = (nlit + 1) / 2;
    BitW a{op};
    for (size_t i = 0; i < n1; ++i) a.put(he.code[lits[i]], len[lits[i]]);
    a.flush();
    const size_t lenA = size_t(a.out - op);
    BitW b{a.out};
    for (size_t i = n1; i < nlit; ++i) b.put(he.code[lits[i]], len[lits[i]]);
    b.flush();
    const size_t lenB = size_t(b.out - a.out);
    wr16(hdr, unsigned(nlit)); wr16(hdr + 2, unsigned(lenA)); wr16(hdr + 4, unsigned(lenB));
    op = b.out;
    std::memcpy(op, seq, ns);
    op += ns;
    return size_t(op - out);
}

bool page3_unhuf(const uint8_t* ip, size_t inlen, uint8_t* dst, size_t outlen) {
    const uint8_t* const iend = ip + inlen;
    uint8_t len[256];
    int sym = 0;
    bool have_hi = false; unsigned cur = 0;
    auto nibble = [&](unsigned& v) -> bool {
        if (!have_hi) { if (ip >= iend) return false; cur = *ip++; v = cur & 15; have_hi = true; return true; }
        v = cur >> 4; have_hi = false; return true;
    };
    std::memset(len, 0, 256);
    while (sym < 256) {
        unsigned v;
        if (!nibble(v)) return false;
        if (v) { len[sym++] = uint8_t(v); continue; }
        unsigned r;
        if (!nibble(r)) return false;
        sym += int(r) + 1;
        if (sym > 256) return false;
    }
    if (iend - ip < 6) return false;
    const size_t nlit = rd16(ip), lenA = rd16(ip + 2), lenB = rd16(ip + 4);
    ip += 6;
    if (nlit == 0 || nlit > outlen || lenA + lenB > size_t(iend - ip)) return false;
    uint16_t tab[1 << HUF_MAXLEN];
    const int tb = huff_table(len, tab);
    if (!tb) return false;
    const unsigned mask = (1u << tb) - 1;
    uint8_t lit[kPage2Max + 32];
    if (nlit > kPage2Max) return false;
    BitR A{ip, ip + lenA, 0, 0, ip};
    BitR B{ip + lenA, ip + lenA + lenB, 0, 0, ip + lenA};
    const size_t n1 = (nlit + 1) / 2, n2 = nlit - n1;
    uint8_t* const la = lit;
    uint8_t* const lb = lit + n1;
    size_t i = 0;
    // each refill leaves >= 56 bits, so 5 symbols of <= 10 bits can be taken per stream before the next one
    for (; i + 5 <= n2; i += 5) {
        A.refill(); B.refill();
        for (int k = 0; k < 5; ++k) {
            const unsigned ea = tab[A.acc & mask]; A.acc >>= ea >> 8; A.nb -= int(ea >> 8); la[i + k] = uint8_t(ea);
            const unsigned eb = tab[B.acc & mask]; B.acc >>= eb >> 8; B.nb -= int(eb >> 8); lb[i + k] = uint8_t(eb);
        }
    }
    for (; i < n1; ++i) {
        A.refill();
        const unsigned ea = tab[A.acc & mask]; A.acc >>= ea >> 8; A.nb -= int(ea >> 8); la[i] = uint8_t(ea);
        if (i < n2) { B.refill(); const unsigned eb = tab[B.acc & mask]; B.acc >>= eb >> 8; B.nb -= int(eb >> 8); lb[i] = uint8_t(eb); }
    }
    if (!A.ok() || !B.ok()) return false;
    std::memset(lit + nlit, 0, 32);
    const uint8_t* seq = ip + lenA + lenB;
    return decode_impl<true, true>(seq, size_t(iend - seq), dst, outlen, lit, lit + nlit);
}

size_t page2_compress(const uint8_t* src, size_t n, uint8_t* dst, int level) {
    if (n > kPage2Max) throw FastError("page too large for the page2 format");
    return level >= 2 ? page2_chain(src, n, dst) : page2_fast(src, n, dst);
}

bool page2_decompress(const uint8_t* src, size_t n, uint8_t* dst, size_t out_len) {
    return decode_impl<true, false>(src, n, dst, out_len, nullptr, nullptr);
}

static bool all_same_bytes(const uint8_t* p, size_t n) {
    for (size_t i = 1; i < n; ++i) if (p[i] != p[0]) return false;
    return n > 0;
}

size_t page3_compress(const uint8_t* page, size_t n, uint8_t* out, int level) {
    if (n == 0) return 0;
    if (all_same_bytes(page, n)) {
        const size_t sz = page[0] == 0 ? 1 : 2;
        if (sz >= n) return 0;                      // never grow: a 1-2 byte "page" stays raw
        out[0] = page[0] == 0 ? PAGE_ZERO : PAGE_FILL;
        out[1] = page[0];
        return sz;
    }
    if (n > kPage2Max) return 0;
    const size_t c = page2_compress(page, n, out + 1, (level == 2 || level == 4) ? 2 : 1);
    size_t best = c + 1;
    out[0] = PAGE_LZ2;
    if (level >= 3 && c > 128) {
        uint8_t tmp[kPage2Max * 2 + 512];
        const size_t h = page3_huf(out + 1, c, tmp + 1);
        if (h && h + 1 < best) { std::memcpy(out + 1, tmp + 1, h); out[0] = PAGE_HUF; best = h + 1; }
    }
    return best < n ? best : 0;
}

bool page3_decompress(const uint8_t* enc, size_t len, uint8_t* page, size_t n) {
    if (len == 0) return false;
    switch (enc[0]) {
        case PAGE_ZERO: if (len != 1) return false; std::memset(page, 0, n); return true;
        case PAGE_FILL: if (len != 2) return false; std::memset(page, enc[1], n); return true;
        case PAGE_LZ2: return n <= kPage2Max && decode_impl<true, false>(enc + 1, len - 1, page, n, nullptr, nullptr);
        case PAGE_HUF: return n <= kPage2Max && page3_unhuf(enc + 1, len - 1, page, n);
        default: return false;
    }
}

namespace {
// Writes mlen bytes at op that repeat the `offset`-byte period ending at op (m == op - offset, offset < mlen).
// Copies one period, then doubles the produced prefix. Every memcpy has disjoint ranges, the produced prefix
// is always a whole number of periods until the final partial chunk, and no slack beyond op + mlen is touched.
inline void expand_period(uint8_t* op, const uint8_t* m, size_t offset, size_t mlen) {
    std::memcpy(op, m, offset);
    size_t made = offset;
    while (made < mlen) {
        const size_t chunk = std::min(made, mlen - made);
        std::memcpy(op + made, op, chunk);
        made += chunk;
    }
}
}  // namespace

namespace {
// V2 = page format: offset byte 0 = repeat the previous offset, 1..0xEF = that offset, 0xF0..0xFF + byte =
// 240 + 12-bit value (covers every offset inside a page of up to 4336 bytes). Everything else matches LZ4.
// SEP: literals come from a separate, already decoded buffer [lp, lend) that is padded by >= 16 readable
// bytes, instead of from the token stream (used by the Huffman-literal page format).
template <bool V2, bool SEP>
bool decode_impl(const uint8_t* ip, size_t inlen, uint8_t* dst, size_t outlen, const uint8_t* lp, const uint8_t* lend) {
    const uint8_t* const iend = ip + inlen;
    uint8_t* op = dst;
    uint8_t* const oend = dst + outlen;
    size_t last = 0;
    (void)last;
    for (;;) {
        if (ip >= iend) return false;
        const unsigned token = *ip++;
        size_t lit = token >> 4;
        // Fast path for the overwhelmingly common sequence (literal run < 15, match length field < 15):
        // with 32 spare input bytes and 64 spare output bytes every copy below is a fixed-size wide copy that
        // cannot leave either buffer, so none of the per-byte bounds checks are needed. Anything else (long
        // lengths, buffer ends, offsets < 8) takes the fully checked path that follows.
        if (lit < 15 && (token & 15) < 15 && iend - ip >= 32 && oend - op >= 64) {
            if (SEP) {
                if (lit > size_t(lend - lp)) return false;
                copy16(op, lp);
                lp += lit;
            } else {
                copy16(op, ip);
                ip += lit;
            }
            op += lit;
            size_t offset;
            if (V2) {
                // branch-free: both candidate readings are computed, then one is selected
                const unsigned o = ip[0], o2 = ip[1];
                const size_t big = ((size_t(o & 15) << 8) | o2) + 240;
                const size_t small = o ? size_t(o) : last;
                const bool two = o >= 0xF0;
                offset = two ? big : small;
                ip += 1 + size_t(two);
                last = offset;
            } else {
                offset = rd16(ip);
                ip += 2;
            }
            if (offset == 0 || offset > size_t(op - dst)) return false;
            const size_t mlen = (token & 15) + MIN_MATCH;      // 4..18
            const uint8_t* m = op - offset;
            if (offset >= 16) {
                copy16(op, m);
                if (mlen > 16) copy16(op + 16, m + 16);
                op += mlen;
                continue;
            }
            if (offset >= 8) {
                copy8(op, m);
                copy8(op + 8, m + 8);
                if (mlen > 16) copy8(op + 16, m + 16);
                op += mlen;
                continue;
            }
            if (offset == 1) {
                std::memset(op, m[0], mlen);
                op += mlen;
                continue;
            }
            for (size_t i = 0; i < mlen; ++i) op[i] = m[i];
            op += mlen;
            continue;
        }
        if (lit == 15) {
            unsigned s;
            do {
                if (ip >= iend) return false;
                s = *ip++;
                lit += s;
                if (lit > outlen) return false;
            } while (s == 255);
        }
        const uint8_t*& L = SEP ? lp : ip;
        const uint8_t* const Lend = SEP ? lend : iend;
        if (lit > size_t(Lend - L) || lit > size_t(oend - op)) return false;
        if (size_t(oend - op) >= lit + 16 && (SEP || size_t(iend - ip) >= lit + 16)) {
            const uint8_t* s = L;
            uint8_t* d = op;
            uint8_t* const e = op + lit;
            do { copy16(d, s); d += 16; s += 16; } while (d < e);
        } else if (lit) {
            std::memcpy(op, L, lit);
        }
        L += lit;
        op += lit;
        if (ip == iend) return op == oend && (!SEP || lp == lend);
        size_t offset;
        if (V2) {
            if (iend - ip < 1) return false;
            const unsigned o = *ip++;
            if (o == 0) offset = last;
            else if (o <= 0xEF) offset = o;
            else {
                if (ip >= iend) return false;
                offset = ((size_t(o & 15) << 8) | *ip++) + 240;
            }
            last = offset;
        } else {
            if (iend - ip < 2) return false;
            offset = rd16(ip);
            ip += 2;
        }
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
            if (offset < mlen && mlen >= 256) {
                // Long overlapping match: expand by doubling (wide memcpy, disjoint ranges, no slack needed).
                expand_period(op, m, offset, mlen);
            } else if (room >= mlen + 16) {
                uint8_t* d = op;
                uint8_t* const e = op + mlen;
                do { copy16(d, m); d += 16; m += 16; } while (d < e);
            } else {
                // Tight output: exact 16-byte chunks are safe for ANY offset >= 16 (each chunk's source lies
                // entirely below its destination), so no output slack is needed. The < 16 byte remainder is
                // disjoint too, because the remainder is shorter than the offset.
                size_t i = 0;
                for (; i + 16 <= mlen; i += 16) copy16(op + i, m + i);
                if (i < mlen) std::memcpy(op + i, m + i, mlen - i);
            }
        } else if (offset == 1) {
            std::memset(op, m[0], mlen);
        } else if (mlen >= 1024 || (room < mlen + 16 && mlen >= 128)) {
            // Long short-period matches benefit from logarithmic expansion even with slack available;
            // medium matches use it when the output is tight.
            expand_period(op, m, offset, mlen);
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
}  // namespace

bool block_decompress(const uint8_t* ip, size_t inlen, uint8_t* dst, size_t outlen) {
    return decode_impl<false, false>(ip, inlen, dst, outlen, nullptr, nullptr);
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
    if (level >= 3) {
        if (n <= kPage2Max) {                       // page formats v2/v3 (not LZ4 compatible), see fast.hpp
            const size_t c = page3_compress(page, n, out, level - 2);
            return (c == 0 || c * 100 > n * size_t(max_pct)) ? 0 : c;
        }
        level = level >= 4 ? 2 : 1;                 // larger pages: fall back to the LZ4-format codec
    }
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
        case PAGE_LZ2:
        case PAGE_HUF:
            return page3_decompress(enc, len, page, n);
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
