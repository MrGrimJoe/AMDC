/* AMDC-F C interface: RAM page compression for any language that can call C (C, C#, Rust, Python ctypes, game engines).
 * Link src/fast.cpp + src/amdcf_c.cpp. All functions are thread-safe unless noted. Little-endian targets only.
 *
 * Levels (higher = smaller pages, slower; decode speed ranges from about 1 GB/s (levels 7-8) to about 3-4 GB/s (levels 1-4) per core on the test machine):
 *   1  fastest, LZ4-compatible output        2  smaller, LZ4-compatible, slower to compress
 *   3-4  LZ with short offsets               5-6  + Huffman literals
 *   7  + split token/offset/length coding, built-in tables     8  + per-page word transforms (smallest)
 * Levels 3-8 use the AMDC-F page formats, which are NOT LZ4 compatible; decode with amdcf_page_decompress.
 */
#ifndef AMDCF_H
#define AMDCF_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

#define AMDCF_LEVEL_MIN 1
#define AMDCF_LEVEL_MAX 8

/* Size of the output buffer amdcf_page_compress needs for a page of `page_size` bytes. */
size_t amdcf_page_bound(size_t page_size);

/* Compresses one page. Returns the encoded size, or 0 when the page is not worth compressing (it would use more than
 * max_pct percent of page_size; use 75 as a default): keep that page uncompressed. All-zero pages cost 1 byte. */
size_t amdcf_page_compress(const void* page, size_t page_size, void* out, int level, unsigned max_pct);

/* Returns 1 when `enc` decodes to exactly page_size bytes, else 0. Memory-safe on arbitrary input. */
int amdcf_page_decompress(const void* enc, size_t enc_len, void* page, size_t page_size);

/* One line describing a level, or NULL for an invalid level. */
const char* amdcf_level_describe(int level);

/* ---- batches on a persistent thread pool (one batch at a time per pool) ---- */
typedef struct amdcf_pool amdcf_pool;
amdcf_pool* amdcf_pool_new(unsigned threads); /* 0 = one thread per hardware thread; NULL on failure */
void amdcf_pool_free(amdcf_pool* pool);
unsigned amdcf_pool_threads(const amdcf_pool* pool);

/* `count` pages back to back at `src`; page i is encoded at dst + i * dst_stride (dst_stride >= amdcf_page_bound(page_size))
 * and out_len[i] receives its encoded size (0 = keep raw). */
void amdcf_pool_compress(amdcf_pool* pool, const void* src, size_t page_size, size_t count, void* dst, size_t dst_stride,
                         size_t* out_len, int level, unsigned max_pct);

/* Decodes blob i (enc[i], enc_len[i]) into dst + i * page_size. Returns the number of pages that failed (0 = all good). */
size_t amdcf_pool_decompress(amdcf_pool* pool, const void* const* enc, const size_t* enc_len, size_t count, void* dst,
                             size_t page_size);

#ifdef __cplusplus
}
#endif
#endif
