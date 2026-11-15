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

```cpp
// 4) many pages at once on all cores (persistent threads; cheap enough to call every frame)
PagePool pool;                                       // 0 = one thread per hardware thread
std::vector<uint8_t> enc(count * page_encode_bound(4096));
std::vector<size_t> len(count);
pool.compress(pages, 4096, count, enc.data(), page_encode_bound(4096), len.data(), /*level*/1);   // len[i] == 0: keep raw
pool.decompress(refs, n_refs, out, 4096);            // returns the number of pages that failed to decode
```

From C, C#, Rust, Python (ctypes) and game engines: `src/amdcf.h` (`amdcf_page_compress`, `amdcf_pool_compress`, ...), built
from `src/fast.cpp` + `src/amdcf_c.cpp`.

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

## Page levels 3-8: a RAM-page format that is smaller than LZ4 (new)

The LZ4 block format spends two bytes on every offset and cannot entropy-code anything, which caps it near 41% on
4 KiB memory pages. For pages (up to kPage2Max = 4336 bytes) there are five more levels in `page_compress` /
`CompressedPageCache`. **These blobs are not LZ4-compatible** (the container and levels 1-2 still are):

| level | what it adds |
|---|---|
| 3 | LZ4-style tokens, but offsets cost 1 byte when < 240 or equal to the previous offset (otherwise 2) |
| 4 | level 3 with a chained, cost-aware parse |
| 5 | level 3 + Huffman-coded literals (canonical code, max length 10, four interleaved bit streams), only when smaller |
| 6 | level 4 + Huffman-coded literals |
| 7 | level 4 + Huffman-coded literals **and** sequence bytes, split into token / offset / length-extension alphabets (kind `PAGE_HUF4`), built-in code tables, see below |
| 8 | level 7 + a per-page **word transform**: the page's 4- or 8-byte words are split into byte planes (optionally after a 32-bit delta) when a cheap trial shows a clear win; the result goes through the level-7 codec |

Each Huffman block is used only when it makes that part smaller, so a page never grows because of it. The sequence bytes
are a large share of a compressed page (33-38% on the compressible heap data here, 6-9% on text and binaries), which is
why level 7 gains about 2 points over level 6.

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
| **amdcf level 7** | 27.7% | 83 | 969 |
| **amdcf level 8** (word transform) | **25.2%** | 70 | 1119 |
| lz4 (fast 1) | 41.3% | 900 | 4323 |
| lz4hc-3 | 37.7% | 208 | 5067 |
| zstd 1 | 27.7% | 387 | 940 |
| zstd 3 | 27.3% | 351 | 970 |
| zlib 1 (stand-in for Windows' Xpress-Huffman, see below) | 29.0% | 130 | 441 |

Honest reading:

* Against **LZ4-class RAM compressors** (what zram uses by default): levels 3/4 are 4-7 points smaller than LZ4 at similar
  compress speed, but **LZ4's decoder is still faster** (about 1.3x faster than level 1, 1.5-1.8x faster than levels 3/4).
* Against **zstd**: level 7 decodes about 6% faster than zstd 1 and is the same size (27.7% vs 27.7% on this corpus); level 8 is 2.5 points smaller and decodes about 23% faster. Both compress
  about 4-5x slower than zstd 1. Levels 5/6 decode about 1.3-1.45x faster than zstd 1 but are 6 and 3 points larger than zstd 1; levels 7 and 8 close that gap.
* Against **Windows' compressor (Xpress-Huffman)**: it is not available outside Windows, so it was **not measured**. zlib-1 is
  the same family (LZ77 + Huffman) and a similar size class, so it is used as a rough proxy only. Level 7 is about
  the same size as zlib-1 and decodes about 2.2x faster; level 6 is 2 points larger and decodes 3.1x faster. Do not read
  this as a measured win over Windows; run it on the target machine.
* **lzo-rle**, zram's other common default, was not built or measured here.
* Compression of the higher levels is slow (levels 7 and 8 about 70-80 MB/s): they suit pages that are written rarely and read often.
* On the three page-cache runs in `amdcf ramsim` (Python objects, text, mixed heap; 288 MiB) the earlier levels saved
  66.7% (level 1), 71.5% (3), 73.9% (5) and 76.8% (6) of RAM.

Multi-core: pages are independent, so both compression and decompression scale with cores (`bench_pages -t N`), as they
do for LZ4 and zstd when those are used per page. On a 2-core VM, 2 threads gave about 1.8-1.9x. Decode in GB/s at 2
threads: level 1 5.7, level 3 4.5, level 5 2.4, level 6 2.6, level 7 1.8, against LZ4 8.3 and zstd 1 1.7. Only 2 threads
were measured; larger counts are not claimed.

Reproduce: `LZ4=/path/to/lz4/lib ZSTD=/path/to/zstd/lib bench/bench_pages.sh [-t THREADS] FILE...` (lz4/zstd optional).

Tests: round trips over 12 page shapes x 10 sizes x 5 levels, a naive reference decoder compared against the fast decoder,
73,000 randomly corrupted blobs checked for guard-byte integrity, all under ASan + UBSan in `make sanitize`.


## Level 7 v2: split alphabets and built-in code tables

Two changes took level 7 from "about the size of zstd 1" to smaller than zstd 1 on three of four held-out data sets.

1. **Split alphabets.** Token bytes, offset bytes and length-extension bytes have very different distributions; coding them
   with one shared Huffman code wasted about 7% of a page (order-0 entropy of the split vs the mixed stream, measured on
   both held-out RAM dumps). Level 7 now codes literals, tokens, offsets and extension bytes as four separate blocks
   (kind `PAGE_HUF4`). The decoder reads the three sequence streams directly (`decode_split`), no re-interleaving pass.
2. **Built-in code tables.** On 4 KiB pages the Huffman code-length headers were about 21% of the compressed output
   (about 96 bytes for literals plus 61 for sequence bytes, of 754 bytes per page). A block can now name one of a few
   built-in codes in 4 bits instead of carrying its own header, and decode tables for them are built once. The encoder
   still picks the cheapest of raw / own code / built-in code per block, so a page never gets bigger. The tables live in
   `src/presets.hpp` and are generated by `tools/train_presets.cpp`.

Retrain the tables on memory that looks like yours (use data that is not your test data):

```
g++ -std=c++17 -O2 -DAMDC_TRAIN -Isrc tools/train_presets.cpp -o build/train_presets
build/train_presets 8 4 4 1 src/presets.hpp -s 2 dump1.bin dump2.bin ...    # literal, token, offset, extension table counts
```

Held-out results (4 KiB pages, single core, same run, shared VM so about +-30% on speed). The tables were trained on a
different Python-object heap, a different compiler process, other shared libraries and other source files, none of which
are in this table. `heap` = writable memory of a Python process holding dicts/lists/strings (83 MB), `cc1` = memory of
a g++ compiler process mid-compile (146 MB), `bin` = a Python interpreter plus libstdc++ (6 MB), `py` = Python source (6 MB).

| data | level 7 before | level 7 now | zstd 1 | LZ4 | level 7 decode now | zstd 1 decode |
|---|---|---|---|---|---|---|
| heap | 17.86% | **17.08%** | 16.50% | 30.83% | 1028 MB/s | 955 MB/s |
| cc1 | n/m | **14.76%** | 16.38% | 25.69% | 908 MB/s | 834 MB/s |
| bin | n/m | **48.58%** | 49.41% | 62.77% | 506 MB/s | 569 MB/s |
| py | n/m | **34.28%** | 35.89% | 50.02% | 513 MB/s | 637 MB/s |

Honest reading: level 7 is now smaller than zstd 1 on cc1, bin and py (by 0.8-1.6 points) and still 0.6 points larger on
the Python heap. It decodes faster than zstd 1 on the two RAM dumps and 10-20% slower on bin and py. Compression is
about 4-5x slower than zstd 1 (70 vs 340 MB/s). LZ4 still decodes about 3x faster, at about twice the size.
The tables are tuned on this machine's kind of data: measure on yours, and retrain if the gain is small there.

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

### Level 8: word transforms (measured on the synthetic corpus below, not on real memory)

Byte-oriented LZ and zstd cannot see that RAM is mostly 4/8-byte words (pointers, counters, indices, floats) whose
high bytes repeat while the low bytes change. Level 8 gathers byte k of every word into one plane (the same idea as
Blosc's shuffle, applied per 4 KiB page) and picks per page among shuffle-8, shuffle-4 and delta-32 + shuffle-4, or none.
Page kind `PAGE_XFORM`: `[7][transform id][inner page blob]`; the decoder decodes the inner blob and inverts the transform.

| file | level 7 | level 8 | zstd 1 |
|---|---|---|---|
| numeric.mem | 63.9% | 51.2% | 62.0% |
| mixed.mem | 16.1% | 13.7% | 15.9% |
| all six files | 27.7% | 25.2% | 27.7% |

Level 8 decodes at about 1,120 MB/s on the whole corpus (zstd 1: 911) and is smaller than zstd 1 there.
Honest reading: the gain comes from word-structured pages (numeric 13 points, mixed 2); text-like pages are unchanged
because the identity transform wins. The corpus is synthetic, so the size of the gain on real memory is unmeasured.
Compression is about 70 MB/s: a cheap run-count screen picks one candidate transform and only that one gets a full pass (an earlier version trialled all three: 2.4 points smaller on numeric, 23 MB/s), so level 8 suits pages that are
written rarely and read often. Tests cover round trips, all three transforms, corruption fuzzing and ASan/UBSan.

## Level 1 on pages: faster compression (new)

For inputs of 8 KiB or less (RAM pages) level 1 now uses a page-specialised compressor with the **same LZ4 output format
and the same parse**: a 16-bit position table that is zeroed per call (instead of 32-bit epoch-tagged entries, which needed
a stale-entry test and a distance test on every probe), four probes per loop iteration, and 16-byte wide copies for
literals instead of a library `memcpy` call. Output size is unchanged (same parse). Measured with `cmpbench`-style
interleaved runs on the synthetic corpus, one core: level 1 compress went from about 0.74x to about 1.04x the speed of
`LZ4_compress_default` (LZ4 fast 1) at the same ratio. The shared VM varies by +-20% between runs, so the claim is
"about at parity or slightly faster", not a large win. Decode is unchanged and still about 0.75x LZ4's decoder
(it executes about 17-20% more instructions than LZ4's decoder on identical blobs).

I also tried a word-dictionary page codec in the style of macOS's WKdm as a speed level. It compressed 1.4-2x faster than
LZ4 on word-structured pages but decoded slower than LZ4 and gave a worse ratio, so it was dropped.

## Picking a level, and using more than one core (new)

| level | use it when | measured size / decode speed (1 core, synthetic corpus) |
|---|---|---|
| 1 | swap-out speed matters most; LZ4-compatible | 41.5% / ~3 GB/s, compresses at about LZ4's speed |
| 2 | LZ4-compatible but smaller | 37.5% / ~3.8 GB/s, compresses ~6x slower than level 1 |
| 3-4 | page format, faster to compress than 2 | 37.2%, 34.4% / 2.2-2.8 GB/s |
| 5-6 | add Huffman literals | 33.3%, 30.9% / ~1.3-1.4 GB/s |
| 7 | smallest LZ-only | 27.7% / ~1 GB/s |
| 8 | smallest; word-structured pages (numbers, pointers, floats) | 25.2% / ~1.1 GB/s, compresses at ~70 MB/s |

Pages are independent, so `PagePool` scales with cores. Measured on the 2-core test VM with `bench/bench_pool.cpp`
(mixed + Python-object + text pages, 302 MB): 1 to 2 threads gave 1.8-2.2x for both directions at every level (for example
level 1 compress 0.81 -> 1.79 GB/s, decode 2.35 -> 4.69 GB/s; level 5 decode 1.27 -> 2.48 GB/s). **Only 2 threads could be
measured**; more cores are expected to scale about linearly until memory bandwidth is the limit, but that is not measured.
There is no GPU path: LZ-family decoding is serial per page and a 4 KiB page is too small to feed a GPU, so none is planned.

Tests: the pool is compared byte-for-byte with sequential `page_compress` for levels 1, 2, 3, 5, 8 and thread counts 1, 2, 3, 8,
failure counting is checked with deliberately corrupted blobs, and four threads share one pool concurrently under
ASan/UBSan and ThreadSanitizer. The C header is compiled as C11 and round-tripped in `tests/test_capi.c`.
