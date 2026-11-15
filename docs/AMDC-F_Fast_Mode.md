# AMDC-F: fast-decode mode

AMDC-1 (context mixing) is built for the smallest archive and decodes at under 1 MB/s. **AMDC-F** is the opposite end of the
trade-off: an LZ77 codec whose decoder runs at **gigabytes per second per core**, meant for data that is decompressed often
or on demand, such as compressed RAM pages, assets, caches and save files. The two modes are separate programs and formats
(`amdc1` vs `amdcf`).

## What it is

* **Block format = LZ4 block format.** Anything `amdcf` writes can be read by any LZ4 block decoder and vice versa (verified
  both directions against the reference `lz4` library, levels 1 and 2, on text, binaries, zeros, random, tiny and empty inputs).
* **Level 1:** fastest compressor (about 0.4–0.8 GB/s). **Level 2:** hash chains plus lazy matching, about 15% smaller output on binaries and source (up to 2× smaller on text-like data),
  about 0.1 GB/s. Decode speed is the same for both: the decoder only follows the tokens.
* **Page codec:** one page in, one tiny blob out. All-zero pages cost 1 byte, single-byte-fill pages 2 bytes, incompressible
  pages are refused (you keep them raw) so compressed RAM never grows.
* **Container (`AMDF`):** independent 256 KiB blocks (configurable), raw / LZ / fill block types, optional 64-bit checksum
  (about 15 GB/s, 4-lane). Blocks encode and decode in parallel (`-j`), output identical for any thread count.
* **Decoder safety:** every length and offset is checked; malformed input returns `false`/throws. 200,000 random mutations of a valid stream
  run under AddressSanitizer + UBSan in the test suite. The container rejects implausible expansion ratios before allocating.

## Measured speed (this machine: 1 vCPU Xeon 2.3 GHz, shared VM, so ±30% run to run)

The lz4/zstd/zlib/xz rows are timed through their Python bindings, which allocate the output buffer on every call and so understate them by up to ~30%; the amdcf rows use a preallocated buffer, as a real RAM decompressor would.
Test input: 1.66 MB of mixed real files (ELF binary, shared library, Python source, JSON, XML). Single thread. Higher is faster.

| codec | size | decode MB/s |
|---|---|---|
| **amdcf level 1**, raw block | 27.3% | ≈ 2,800 |
| amdcf level 1, container + checksum verified | 27.3% | ≈ 2,300 |
| **amdcf level 2**, raw block | 23.5% | ≈ 2,200–3,600 |
| lz4 hc-9 (reference library) | 22.5% | ≈ 4,300 |
| zstd-1 | 18.9% | ≈ 2,000 |
| zstd-19 | 14.6% | ≈ 1,300 |
| zlib-6 | 17.4% | ≈ 550 |
| xz-6 | 13.9% | ≈ 140 |
| *(AMDC-1 context mixing, for comparison)* | *≈ 13%* | *≈ 0.65* |

Honest reading: AMDC-F decodes **~5× faster than zlib, ~20× faster than xz and thousands of times faster than AMDC-1**, and in the same class as
LZ4 itself. It is **not** faster than the LZ4 reference decoder (in the last run lz4 hc-9 was faster), and zstd compresses noticeably
smaller at similar speed. Choose AMDC-F when you want a small, dependency-free, LZ4-compatible codec that you own and can embed;
choose zstd/LZ4 when you want the most mature library.

Page level (`amdcf ramsim`, 4 KiB pages, same input): **66.8% of RAM saved** at level 1 (70.3% at level 2); page-in (random order, including
hash lookup and byte-for-byte verify) **≈ 1.2–1.5 µs per page ≈ 2.8–3.4 GB/s**; page-out ≈ 5 µs per page at level 1. On a
memory image with 96 of 448 pages all-zero the saving rises to 74.3%. Real RAM contents vary a lot; measure your own.

## Using it

```cpp
#include "fast.hpp"
#include "pagecache.hpp"
using namespace amdc1::fast;

// 1) keep pages compressed in RAM, thread-safe
CompressedPageCache cache(4096);                    // page size, level 1
cache.put(page_id, page_ptr);                       // returns bytes of RAM actually used
cache.get(page_id, out_ptr);                        // decompresses into your buffer

// 2) raw page codec, no allocation
uint8_t enc[page_encode_bound(4096)];
size_t n = page_compress(page, 4096, enc);          // 0 => not worth compressing, keep raw
bool ok = page_decompress(enc, n, page_out, 4096);

// 3) whole buffers / files, multi-threaded, checksummed
Bytes z = compress(data, kDefaultBlock, /*level*/2, /*threads*/0);
decompress_into(z.data(), z.size(), dst, dst_cap, /*threads*/8);   // allocation-free
```

CLI: `amdcf compress|decompress|bench|ramsim ...` (see `amdcf` with no arguments). Build with `make` (Linux/macOS),
`make windows` (MinGW-w64 cross-build) or CMake (`cmake -B build && cmake --build build --config Release`, works with MSVC).
Little-endian targets only, which covers every Windows target (x64 and ARM64).

## Verification

* `make test` runs `test_fast` (141k checks: round trips, a differential test of the decoder against a deliberately naive reference
  decoder on random valid streams and on mutated blocks, and a multi-threaded page-cache stress test) plus the Python cross-check.
* `make sanitize` (ASan + UBSan) and `make tsan` (ThreadSanitizer) run the same suite.
* `make crosscheck-fast` checks the C++ codec against `reference/amdcf/`, an independent pure-Python implementation: archives from either
  side must decode identically in the other, and mutated archives must be accepted or rejected identically by both.

## Decoder microbenchmark (synthetic)

`make bench-fast` decodes one block with a long short-period match (offsets 2 to 64) over and over from cache. It shows 40-80 GB/s
because `memcpy`-based expansion of a long repeat is the decoder's best case. **This is not real-data throughput**; for that, use the
table above (about 2-4 GB/s per core on typical data). Quote the two separately.

## Page levels 3-7: a RAM-page format that is smaller than LZ4 (new)

The LZ4 block format spends two bytes on every offset and cannot entropy-code anything, which caps it near 41% on
4 KiB memory pages. For pages (up to kPage2Max = 4336 bytes) there are five more levels in `page_compress` /
`CompressedPageCache`. **These blobs are not LZ4-compatible** (the container and levels 1-2 still are):

| level | what it adds |
|---|---|
| 3 | LZ4-style tokens, but offsets cost 1 byte when < 240 or equal to the previous offset (otherwise 2) |
| 4 | level 3 with a chained, cost-aware parse |
| 5 | level 3 + Huffman-coded literals (canonical code, max length 10, four interleaved bit streams), only when smaller |
| 6 | level 4 + Huffman-coded literals |
| 7 | level 4, with the page split into four streams (literals, tokens, offsets, length extensions) that each get their own Huffman code when that is smaller |

Each Huffman block is used only when it makes that part smaller, so a page never grows because of it. The sequence bytes
(tokens, offsets, lengths) are a large share of a compressed page (33-38% on the compressible heap data here, 6-9% on
text and binaries), which is why level 7 gains about 2.4 points over level 6. Splitting them into one stream per kind
(rather than one shared code) was worth only a further 0.24 points for about 10% of decode speed: each extra stream pays
for its own code table, which is a lot on pages that compress to a few hundred bytes.

Per-page results, 4 KiB pages compressed independently, single core, every page decoded and compared. Corpus: 383 MB of
heap images generated here (Python object graphs, text, numeric arrays, a mixed heap with zero and random pages) plus
binaries; **not** dumps of a real Windows or Linux machine, so measure your own. LZ4 and zstd were built from current
upstream sources, zstd with reused contexts (as zswap does). Same-filled pages count as 8 bytes for every codec.
The shared VM varies by about +-30% between runs, so compare rows within one run.

| codec | size | compress MB/s | decompress MB/s |
|---|---|---|---|
| amdcf level 1 (LZ4 format) | 41.5% | 715 | 3208 |
| amdcf level 2 (LZ4 format) | 37.5% | 133 | 4001 |
| **amdcf level 3** | 37.2% | 669 | 2372 |
| **amdcf level 4** | 34.4% | 137 | 2858 |
| **amdcf level 5** | 33.5% | 255 | 1256 |
| **amdcf level 6** | 31.0% | 103 | 1378 |
| **amdcf level 7** | 28.6% | 71 | 871 |
| lz4 (fast 1) | 41.3% | 900 | 4323 |
| lz4hc-3 | 37.7% | 208 | 5067 |
| zstd 1 | 27.7% | 387 | 940 |
| zstd 3 | 27.3% | 351 | 970 |
| zlib 1 (stand-in for Windows' Xpress-Huffman, see below) | 29.0% | 130 | 441 |

Honest reading:

* Against **LZ4-class RAM compressors** (what zram uses by default): levels 3/4 are 4-7 points smaller than LZ4 at similar
  compress speed, but **LZ4's decoder is still faster** (about 1.3x faster than level 1, 1.5-1.8x faster than levels 3/4).
* Against **zstd**: level 7 is **about 0.9 points larger and about 7% slower to decode** than zstd 1, and it compresses
  about 5x slower, so zstd 1 is the better codec at that size. Per file, level 7 is within 0.3-0.5 points of zstd 1 on
  the mixed heap and text, 0.9-1.0 on Python objects and binaries, and 2.3 behind on numeric arrays. Levels 5/6 decode about 1.3-1.45x faster than zstd 1 but are 6 and 3 points larger. zstd still wins on size.
* Against **Windows' compressor (Xpress-Huffman)**: it is not available outside Windows, so it was **not measured**. zlib-1 is
  the same family (LZ77 + Huffman) and a similar size class, so it is used as a rough proxy only. Level 7 is slightly
  smaller than zlib-1 and decodes about 2x faster; level 6 is 2 points larger and decodes 3.1x faster. Do not read
  this as a measured win over Windows; run it on the target machine.
* **lzo-rle**, zram's other common default, was not built or measured here.
* Compression of the higher levels is slow (level 7 about 90 MB/s): they suit pages that are written rarely and read often.
* On the three page-cache runs in `amdcf ramsim` (Python objects, text, mixed heap; 288 MiB) the earlier levels saved
  66.7% (level 1), 71.5% (3), 73.9% (5) and 76.8% (6) of RAM.

Multi-core: pages are independent, so both compression and decompression scale with cores (`bench_pages -t N`), as they
do for LZ4 and zstd when those are used per page. On a 2-core VM, 2 threads gave about 1.8-1.9x. Decode in GB/s at 2
threads: level 1 5.9, level 3 4.5, level 5 2.3, level 6 2.6, level 7 1.6, against LZ4 8.1 and zstd 1 1.8. Only 2 threads
were measured; larger counts are not claimed.

Reproduce: `LZ4=/path/to/lz4/lib ZSTD=/path/to/zstd/lib bench/bench_pages.sh [-t THREADS] FILE...` (lz4/zstd optional).

Tests: round trips over 12 page shapes x 10 sizes x 5 page levels, a naive reference decoder compared against the fast decoder,
73,000 randomly corrupted blobs checked for guard-byte integrity, all under ASan + UBSan in `make sanitize`.

## What this does NOT do: "optimizing Windows by compressing its RAM"

Be clear about the limits before relying on this for that goal:

* **Windows 10/11 already compresses RAM** (the built-in memory compression store). A normal program cannot take other
  processes' memory and re-store it in another format: that needs a **kernel-mode driver** (paging hooks, signed drivers,
  crash-safety guarantees). That is a different, high-risk project and is not included here.
* What *is* delivered is the user-space building block: a fast, safe page compressor and a thread-safe compressed page cache you can build into **your own**
  applications (a game's asset cache, an editor's undo history, an in-memory database, a compressed RAM-disk) so *your* memory footprint
  shrinks by roughly 2–3× on typical data at ~1 µs per page access.
* Compression costs CPU. It pays off when memory is the bottleneck (swapping to disk is ~100× slower than decompressing), not on a machine with RAM to spare.
* The Windows executables were cross-compiled with MinGW-w64 and **run under Wine** here (all 1,378 tests pass, round-trips exact, archives
  interchangeable with the Linux build). They have not been run on a physical Windows machine; run `test_fast.exe` first on yours.

## Format summary (`AMDF` v1)

```
"AMDF" | 1 | flags u8 (bit0 = checksum) | orig_len | block_size | blocks... | [hash64 u64 LE]
block: 0 | raw bytes (block_len)      1 | varint clen | LZ4 block      2 | fill byte
```
Varints are unsigned LEB128. Block lengths are implied (`block_size`, last block shorter). `hash64` is a 4-lane multiply/xorshift
hash of the original data (error detection, not cryptographic).
