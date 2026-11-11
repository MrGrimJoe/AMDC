#include "transforms.hpp"

#include <algorithm>
#include <cstring>

namespace amdc1 {

Bytes xf_apply(int xf, uint32_t k, const uint8_t* d, size_t n) {
    Bytes o(d, d + n);
    switch (xf) {
        case XF_ID: break;
        case XF_DELTA:
            for (size_t t = n; t-- > k;) o[t] = uint8_t(d[t] - d[t - k]);
            break;
        case XF_XOR:
            for (size_t t = n; t-- > k;) o[t] = d[t] ^ d[t - k];
            break;
        case XF_SHUFFLE: {
            size_t m = n - n % k, q = 0;
            for (size_t j = 0; j < k; ++j)
                for (size_t i = j; i < m; i += k) o[q++] = d[i];
            break;
        }
        case XF_X86: {
            // E8/E9 call/jump targets: relative -> absolute on the low 24 bits.
            // The top byte (00/FF) is the trigger and is never modified. An opcode that was examined but not converted
            // blocks conversion for the next 3 positions, so a trigger byte is never rewritten by a later conversion
            // and both directions make identical decisions.
            long lf = -10;
            for (size_t i = 0; i + 4 < n;) {
                const bool op = d[i] == 0xE8 || d[i] == 0xE9;
                if (op && long(i) - lf > 3 && (d[i + 4] == 0x00 || d[i + 4] == 0xFF)) {
                    uint32_t v = uint32_t(d[i + 1]) | uint32_t(d[i + 2]) << 8 | uint32_t(d[i + 3]) << 16;
                    uint32_t a = (v + uint32_t(i + 5)) & 0xFFFFFFu;
                    o[i + 1] = uint8_t(a); o[i + 2] = uint8_t(a >> 8); o[i + 3] = uint8_t(a >> 16);
                    i += 5;
                } else {
                    if (op) lf = long(i);
                    ++i;
                }
            }
            break;
        }
        default: throw std::invalid_argument("unknown transform");
    }
    return o;
}

Bytes xf_invert(int xf, uint32_t k, const uint8_t* d, size_t n) {
    Bytes o(d, d + n);
    switch (xf) {
        case XF_ID: break;
        case XF_DELTA:
            for (size_t t = k; t < n; ++t) o[t] = uint8_t(o[t] + o[t - k]);
            break;
        case XF_XOR:
            for (size_t t = k; t < n; ++t) o[t] ^= o[t - k];
            break;
        case XF_SHUFFLE: {
            size_t m = n - n % k, q = 0;
            for (size_t j = 0; j < k; ++j)
                for (size_t i = j; i < m; i += k) o[i] = d[q++];
            break;
        }
        case XF_X86: {
            // E8/E9 call/jump targets: absolute -> relative on the low 24 bits.
            // The top byte (00/FF) is the trigger and is never modified. An opcode that was examined but not converted
            // blocks conversion for the next 3 positions, so a trigger byte is never rewritten by a later conversion
            // and both directions make identical decisions.
            long lf = -10;
            for (size_t i = 0; i + 4 < n;) {
                const bool op = o[i] == 0xE8 || o[i] == 0xE9;
                if (op && long(i) - lf > 3 && (o[i + 4] == 0x00 || o[i + 4] == 0xFF)) {
                    uint32_t v = uint32_t(o[i + 1]) | uint32_t(o[i + 2]) << 8 | uint32_t(o[i + 3]) << 16;
                    uint32_t a = (v - uint32_t(i + 5)) & 0xFFFFFFu;
                    o[i + 1] = uint8_t(a); o[i + 2] = uint8_t(a >> 8); o[i + 3] = uint8_t(a >> 16);
                    i += 5;
                } else {
                    if (op) lf = long(i);
                    ++i;
                }
            }
            break;
        }
        default: throw std::invalid_argument("unknown transform");
    }
    return o;
}

static bool ws(uint8_t c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

bool parse_pnm(const uint8_t* d, size_t n, PnmInfo& out) {
    if (n < 3 || d[0] != 'P' || (d[1] != '5' && d[1] != '6') || !ws(d[2])) return false;
    uint32_t ch = d[1] == '5' ? 1 : 3;
    size_t pos = 2;
    uint64_t vals[3];
    int nv = 0;
    while (nv < 3) {
        while (pos < n && ws(d[pos])) ++pos;
        if (pos < n && d[pos] == '#') {
            while (pos < n && d[pos] != '\r' && d[pos] != '\n') ++pos;
            continue;
        }
        size_t st = pos;
        while (pos < n && d[pos] >= '0' && d[pos] <= '9') ++pos;
        if (pos == st || pos - st > 9) return false;
        uint64_t v = 0;
        for (size_t i = st; i < pos; ++i) v = v * 10 + (d[i] - '0');
        vals[nv++] = v;
    }
    if (pos >= n || !ws(d[pos])) return false;
    ++pos;
    if (vals[2] != 255 || vals[0] < 1 || vals[1] < 1) return false;
    if (pos + vals[0] * vals[1] * ch > n) return false;
    out = {pos, uint32_t(vals[0]), uint32_t(vals[1]), ch};
    return true;
}

namespace {
inline int fold(int e) { return e < 128 ? 2 * e : 511 - 2 * e; }
inline int unfold(int z) { return (z & 1) ? (511 - z) >> 1 : z >> 1; }
inline int pred(const uint8_t* pl, size_t i, size_t x, size_t y, size_t w) {
    if (y == 0) return x ? pl[i - 1] : 0;
    if (x == 0) return pl[i - w];
    int a = pl[i - 1], b = pl[i - w], c = pl[i - w - 1];
    if (c >= std::max(a, b)) return std::min(a, b);
    if (c <= std::min(a, b)) return std::max(a, b);
    return a + b - c;
}
void med_res(const uint8_t* pl, size_t w, size_t h, uint8_t* res) {
    for (size_t y = 0; y < h; ++y)
        for (size_t x = 0; x < w; ++x) {
            size_t i = y * w + x;
            res[i] = uint8_t(fold((pl[i] - pred(pl, i, x, y, w)) & 255));
        }
}
void med_inv(const uint8_t* res, size_t w, size_t h, uint8_t* pl) {
    for (size_t y = 0; y < h; ++y)
        for (size_t x = 0; x < w; ++x) {
            size_t i = y * w + x;
            pl[i] = uint8_t((pred(pl, i, x, y, w) + unfold(res[i])) & 255);
        }
}
}  // namespace

Bytes image_fwd(const uint8_t* d, size_t n, const PnmInfo& pi, int ct) {
    size_t pn = size_t(pi.w) * pi.h, tot = pn * pi.ch;
    std::vector<Bytes> planes(pi.ch, Bytes(pn));
    for (size_t i = 0; i < pn; ++i)
        for (uint32_t c = 0; c < pi.ch; ++c) planes[c][i] = d[pi.hdr + i * pi.ch + c];
    if (ct == 1)
        for (size_t i = 0; i < pn; ++i) {
            uint8_t g = planes[1][i];
            planes[0][i] = uint8_t(planes[0][i] - g);
            planes[2][i] = uint8_t(planes[2][i] - g);
        }
    Bytes o(d, d + n);
    for (uint32_t c = 0; c < pi.ch; ++c) med_res(planes[c].data(), pi.w, pi.h, &o[pi.hdr + c * pn]);
    (void)tot;
    return o;
}

Bytes image_inv(const uint8_t* y, size_t n, const PnmInfo& pi, int ct) {
    size_t pn = size_t(pi.w) * pi.h;
    std::vector<Bytes> planes(pi.ch, Bytes(pn));
    for (uint32_t c = 0; c < pi.ch; ++c) med_inv(y + pi.hdr + c * pn, pi.w, pi.h, planes[c].data());
    if (ct == 1)
        for (size_t i = 0; i < pn; ++i) {
            uint8_t g = planes[1][i];
            planes[0][i] = uint8_t(planes[0][i] + g);
            planes[2][i] = uint8_t(planes[2][i] + g);
        }
    Bytes o(y, y + n);
    for (size_t i = 0; i < pn; ++i)
        for (uint32_t c = 0; c < pi.ch; ++c) o[pi.hdr + i * pi.ch + c] = planes[c][i];
    return o;
}

}  // namespace amdc1
