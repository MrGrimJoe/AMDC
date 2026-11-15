// C interface for AMDC-F page compression (see amdcf.h).
#include "amdcf.h"

#include <new>
#include <vector>

#include "fast.hpp"

using namespace amdc1::fast;

struct amdcf_pool {
    PagePool pool;
    explicit amdcf_pool(unsigned t) : pool(t) {}
};

extern "C" {

size_t amdcf_page_bound(size_t page_size) { return page_encode_bound(page_size); }

size_t amdcf_page_compress(const void* page, size_t page_size, void* out, int level, unsigned max_pct) {
    if (!page || !out || level < AMDCF_LEVEL_MIN || level > AMDCF_LEVEL_MAX) return 0;
    return page_compress(static_cast<const uint8_t*>(page), page_size, static_cast<uint8_t*>(out), level, max_pct);
}

int amdcf_page_decompress(const void* enc, size_t enc_len, void* page, size_t page_size) {
    if (!enc || !page) return 0;
    return page_decompress(static_cast<const uint8_t*>(enc), enc_len, static_cast<uint8_t*>(page), page_size) ? 1 : 0;
}

const char* amdcf_level_describe(int level) {
    switch (level) {
        case 1: return "1: fastest; LZ4-compatible output";
        case 2: return "2: LZ4-compatible, chained matches (smaller, slower to compress)";
        case 3: return "3: LZ with short offsets, fast parse";
        case 4: return "4: LZ with short offsets, chained parse";
        case 5: return "5: level 3 + Huffman-coded literals";
        case 6: return "6: level 4 + Huffman-coded literals";
        case 7: return "7: level 6 + split token/offset/length coding with built-in tables";
        case 8: return "8: level 7 + per-page word transforms (smallest; slowest to compress)";
        default: return nullptr;
    }
}

amdcf_pool* amdcf_pool_new(unsigned threads) {
    try { return new amdcf_pool(threads); } catch (...) { return nullptr; }
}
void amdcf_pool_free(amdcf_pool* p) { delete p; }
unsigned amdcf_pool_threads(const amdcf_pool* p) { return p ? p->pool.threads() : 0; }

void amdcf_pool_compress(amdcf_pool* p, const void* src, size_t page_size, size_t count, void* dst, size_t dst_stride,
                         size_t* out_len, int level, unsigned max_pct) {
    if (!p || !src || !dst || !out_len || level < AMDCF_LEVEL_MIN || level > AMDCF_LEVEL_MAX) {
        if (out_len) for (size_t i = 0; i < count; ++i) out_len[i] = 0;
        return;
    }
    p->pool.compress(static_cast<const uint8_t*>(src), page_size, count, static_cast<uint8_t*>(dst), dst_stride, out_len, level, max_pct);
}

size_t amdcf_pool_decompress(amdcf_pool* p, const void* const* enc, const size_t* enc_len, size_t count, void* dst, size_t page_size) {
    if (!p || !enc || !enc_len || !dst) return count;
    try {
        std::vector<PageRef> refs(count);
        for (size_t i = 0; i < count; ++i) refs[i] = PageRef{static_cast<const uint8_t*>(enc[i]), enc_len[i]};
        return p->pool.decompress(refs.data(), count, static_cast<uint8_t*>(dst), page_size);
    } catch (...) { return count; }
}

}  // extern "C"
