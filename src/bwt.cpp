#include "bwt.hpp"

#include <algorithm>
#include <cstring>

namespace amdc1 {
namespace bwt {
namespace {

// ---- SA-IS (Nong, Zhang, Chan). s has n symbols over [0,K) and ends with a unique smallest symbol (0).
template <class T>
void get_buckets(const T* s, int* bkt, int n, int K, bool end) {
    std::fill(bkt, bkt + K, 0);
    for (int i = 0; i < n; ++i) ++bkt[s[i]];
    int sum = 0;
    for (int i = 0; i < K; ++i) {
        sum += bkt[i];
        bkt[i] = end ? sum : sum - bkt[i];
    }
}
inline bool is_lms(const std::vector<char>& t, int i) { return i > 0 && t[i] && !t[i - 1]; }

template <class T>
void induce_l(const std::vector<char>& t, int* SA, const T* s, int* bkt, int n, int K) {
    get_buckets(s, bkt, n, K, false);
    for (int i = 0; i < n; ++i) {
        int j = SA[i] - 1;
        if (SA[i] > 0 && !t[j]) SA[bkt[s[j]]++] = j;
    }
}
template <class T>
void induce_s(const std::vector<char>& t, int* SA, const T* s, int* bkt, int n, int K) {
    get_buckets(s, bkt, n, K, true);
    for (int i = n - 1; i >= 0; --i) {
        int j = SA[i] - 1;
        if (SA[i] > 0 && t[j]) SA[--bkt[s[j]]] = j;
    }
}

template <class T>
void sais(const T* s, int* SA, int n, int K) {
    std::vector<char> t(static_cast<size_t>(n));
    t[n - 1] = 1;
    if (n >= 2) t[n - 2] = 0;
    for (int i = n - 3; i >= 0; --i) t[i] = (s[i] < s[i + 1] || (s[i] == s[i + 1] && t[i + 1])) ? 1 : 0;
    std::vector<int> bkt(static_cast<size_t>(K));
    get_buckets(s, bkt.data(), n, K, true);
    std::fill(SA, SA + n, -1);
    for (int i = 1; i < n; ++i)
        if (is_lms(t, i)) SA[--bkt[s[i]]] = i;
    induce_l(t, SA, s, bkt.data(), n, K);
    induce_s(t, SA, s, bkt.data(), n, K);
    int n1 = 0;
    for (int i = 0; i < n; ++i)
        if (is_lms(t, SA[i])) SA[n1++] = SA[i];
    std::fill(SA + n1, SA + n, -1);
    int name = 0, prev = -1;
    for (int i = 0; i < n1; ++i) {
        int pos = SA[i];
        bool diff = false;
        for (int d = 0; d < n; ++d) {
            if (prev == -1 || s[pos + d] != s[prev + d] || t[pos + d] != t[prev + d]) { diff = true; break; }
            if (d > 0 && (is_lms(t, pos + d) || is_lms(t, prev + d))) break;
        }
        if (diff) { ++name; prev = pos; }
        SA[n1 + pos / 2] = name - 1;
    }
    for (int i = n - 1, j = n - 1; i >= n1; --i)
        if (SA[i] >= 0) SA[j--] = SA[i];
    int* SA1 = SA;
    int* s1 = SA + n - n1;
    if (name < n1) sais<int>(s1, SA1, n1, name);
    else
        for (int i = 0; i < n1; ++i) SA1[s1[i]] = i;
    get_buckets(s, bkt.data(), n, K, true);
    for (int i = 1, j = 0; i < n; ++i)
        if (is_lms(t, i)) s1[j++] = i;
    for (int i = 0; i < n1; ++i) SA1[i] = s1[SA1[i]];
    std::fill(SA + n1, SA + n, -1);
    for (int i = n1 - 1; i >= 0; --i) {
        int j = SA[i];
        SA[i] = -1;
        SA[--bkt[s[j]]] = j;
    }
    induce_l(t, SA, s, bkt.data(), n, K);
    induce_s(t, SA, s, bkt.data(), n, K);
}

}  // namespace

void forward(const uint8_t* in, size_t n, uint8_t* L, Header& h) {
    const int N = int(n) + 1;
    std::vector<uint16_t> s(static_cast<size_t>(N));
    for (size_t i = 0; i < n; ++i) s[i] = uint16_t(in[i] + 1);
    s[n] = 0;
    std::vector<int> SA(static_cast<size_t>(N));
    sais<uint16_t>(s.data(), SA.data(), N, 257);
    int bnd[kChains - 1];
    for (int j = 0; j < kChains - 1; ++j) bnd[j] = int((size_t(j + 1) * n) / kChains);
    size_t o = 0;
    h = Header();
    for (int r = 0; r < N; ++r) {
        const int pos = SA[r];
        if (pos == 0) { h.primary = uint32_t(r); continue; }
        L[o++] = in[pos - 1];
    }
    // starting rows of chains 1..kChains-1: the rows of the suffixes at the segment boundaries
    for (int r = 0; r < N; ++r)
        for (int j = 0; j < kChains - 1; ++j)
            if (SA[r] == bnd[j]) h.starts[j] = uint32_t(r);
}

bool inverse(const uint8_t* L, size_t n, const Header& h, uint8_t* out) {
    if (n == 0 || n > kMaxBlock) return false;
    const size_t N = n + 1;
    if (h.primary >= N) return false;
    for (int j = 0; j < kChains - 1; ++j)
        if (h.starts[j] >= N) return false;
    uint32_t cnt[256] = {};
    for (size_t i = 0; i < n; ++i) ++cnt[L[i]];
    uint32_t next[256];
    uint32_t sum = 1;                     // row 0 is the end-marker suffix
    for (int c = 0; c < 256; ++c) { next[c] = sum; sum += cnt[c]; }
    std::vector<uint32_t> cell(N);
    // cell[r] = (LF(r) << 8) | symbol(r)
    {
        size_t li = 0;
        for (size_t r = 0; r < N; ++r) {
            if (r == h.primary) { cell[r] = 0; continue; }
            const uint8_t c = L[li++];
            cell[r] = (next[c]++ << 8) | c;
        }
    }
    size_t b[kChains + 1];
    b[0] = 0;
    for (int j = 0; j < kChains - 1; ++j) b[j + 1] = (size_t(j + 1) * n) / kChains;
    b[kChains] = n;
    uint32_t row[kChains];
    size_t pos[kChains];                  // next output index (written downwards)
    size_t len[kChains];
    row[kChains - 1] = 0;                 // last chain starts at the end-marker suffix
    for (int j = 0; j < kChains - 1; ++j) row[j] = h.starts[j];
    size_t minlen = n;
    for (int j = 0; j < kChains; ++j) {
        pos[j] = b[j + 1];
        len[j] = b[j + 1] - b[j];
        minlen = std::min(minlen, len[j]);
    }
    for (size_t step = 0; step < minlen; ++step) {
        uint32_t c0 = cell[row[0]], c1 = cell[row[1]], c2 = cell[row[2]], c3 = cell[row[3]];
        out[--pos[0]] = uint8_t(c0); row[0] = c0 >> 8;
        out[--pos[1]] = uint8_t(c1); row[1] = c1 >> 8;
        out[--pos[2]] = uint8_t(c2); row[2] = c2 >> 8;
        out[--pos[3]] = uint8_t(c3); row[3] = c3 >> 8;
    }
    for (int j = 0; j < kChains; ++j)
        for (size_t step = minlen; step < len[j]; ++step) {
            const uint32_t c = cell[row[j]];
            out[--pos[j]] = uint8_t(c);
            row[j] = c >> 8;
        }
    return true;
}

}  // namespace bwt
}  // namespace amdc1
