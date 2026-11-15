/* Compiled as C: checks that amdcf.h is a valid C header and the C interface round-trips. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/amdcf.h"

int main(void) {
    enum { P = 4096, N = 200 };
    unsigned char* mem = malloc((size_t)P * N);
    for (size_t i = 0; i < (size_t)P * N; ++i) mem[i] = (unsigned char)((i / 7) % 13 + (i % 4096 < 2000 ? 0 : (i * 31) % 3));
    memset(mem, 0, P);                                 /* page 0: all zero */
    int fails = 0;
    for (int level = AMDCF_LEVEL_MIN; level <= AMDCF_LEVEL_MAX; ++level) {
        if (!amdcf_level_describe(level)) ++fails;
        size_t stride = amdcf_page_bound(P);
        unsigned char* enc = malloc(stride * N);
        size_t len[N];
        amdcf_pool* pool = amdcf_pool_new(3);
        if (!pool || amdcf_pool_threads(pool) != 3) { ++fails; break; }
        amdcf_pool_compress(pool, mem, P, N, enc, stride, len, level, 75);
        const void* ptrs[N]; size_t lens[N]; size_t idx[N]; size_t m = 0;
        for (size_t i = 0; i < N; ++i) if (len[i]) { ptrs[m] = enc + i * stride; lens[m] = len[i]; idx[m++] = i; }
        unsigned char* out = malloc((size_t)P * N);
        if (amdcf_pool_decompress(pool, ptrs, lens, m, out, P) != 0) ++fails;
        for (size_t k = 0; k < m; ++k) if (memcmp(out + k * P, mem + idx[k] * P, P)) ++fails;
        /* single-page path agrees with the batch path */
        unsigned char one[8192], back[P];
        size_t c = amdcf_page_compress(mem + P, P, one, level, 75);
        if (c != len[1]) ++fails;
        if (c && (!amdcf_page_decompress(one, c, back, P) || memcmp(back, mem + P, P))) ++fails;
        if (len[0] != 1 || !amdcf_page_decompress(enc, 1, back, P) || back[17] != 0) ++fails;   /* zero page = 1 byte */
        if (amdcf_page_compress(mem, P, one, 99, 75) != 0) ++fails;                              /* bad level rejected */
        amdcf_pool_free(pool); free(enc); free(out);
    }
    if (amdcf_level_describe(0) || amdcf_level_describe(9)) ++fails;
    printf("capi: %d failures\n", fails);
    free(mem);
    return fails ? 1 : 0;
}
