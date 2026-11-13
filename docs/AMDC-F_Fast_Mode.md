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
