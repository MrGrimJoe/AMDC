# AMDC-1: Adaptive Multi-Model Data Compression

Three lossless codecs in one repo.

* **`amdc1`: maximum ratio** (context mixing, format v4). Spec: `docs/AMDC-1_Specification_v1.4.md`.
* **`amdcb`: middle tier, experimental** (BWT + context-mixed ranks), about 5 MB/s compress and 10 MB/s decompress per core, ratio between bzip2 and xz. See `docs/AMDC-B_BWT_Tier.md`.
* **`amdcf`: maximum decode speed** (LZ4-compatible LZ77), ~2-4 GB/s per core, with a RAM page codec and compressed page cache. See `docs/AMDC-F_Fast_Mode.md`.

`amdc1`: reversible transforms (delta/xor/shuffle, x86 call filter, image residuals, automatic record-length detection) + 15 hashed context
models with check bytes (12 with bit-history states) + 2 match models, mixed by a two-layer logistic mixer network, refined by two APM stages and coded with a
16-bit-probability binary arithmetic coder. C++17, no dependencies.

```
make                      # builds amdc1, amdcf, amdcb and the test programs
make test                 # unit checks, C++/Python cross-checks (amdc1 byte-identity, amdcf interoperability)
make sanitize             # fast codec under ASan/UBSan, incl. decoder fuzz and differential tests
make tsan                 # page cache under ThreadSanitizer (concurrent put/get/erase)
make crosscheck-fast      # amdcf C++ vs the pure-Python reference in reference/amdcf/
make bench-fast           # synthetic match-copy decoder microbenchmark (best case, not real-data speed)
make windows              # amdcf.exe / amdc1.exe / test_fast.exe via MinGW-w64
build/amdc1 compress   [-b BYTES] [-l 0|1|2] [-m BITS] [-j N] [--verify] IN OUT
build/amdc1 decompress [--max-output BYTES] [-j N] IN OUT
build/amdcb compress   [-b BYTES] [-j N] [--verify] IN OUT
build/amdcb decompress [--max-output BYTES] [-j N] IN OUT
python3 bench/bench_real.py --files DIR   # vs gzip/bzip2/xz/7z/zstd/brotli/lz4/zpaq, every result round-trip verified
tools/ablate.sh DIR                       # size impact of removing each model input
```

* `-l 0` lite model (about 3.5x faster), `-l 1` full model + sampled transform search (default), `-l 2` exhaustive search.
* `-m BITS` bounds memory (16..24, default 22). `-l 0 -m 20` runs in about 59 MB, `-l 1 -m 20` in about 111 MB; the default needs about 120 MB (lite) / 270 MB (full).
* Blocks default to 16 MiB (larger blocks share more context; independent 1 MiB blocks cost about 4.6% on text). Blocks are independent and processed in parallel with `-j`; output is identical for any `-j`.
* 8-bit PGM/PPM files get a dedicated lossless image path and are reproduced byte-for-byte.
* `reference/amdcf/` is an independent pure-Python AMDC-F (blocks, pages, container); `make crosscheck-fast` checks it against the C++ codec in both directions.
- `reference/amdc1/` is an independent pure-Python implementation of the same format (`PYTHONPATH=reference python3 -m amdc1 ...`); `make test` checks that both produce byte-identical archives.

* `amdcs` (optional, experimental): a solid multi-file archive mode for AMDC-F that lets the LZ4-format blocks match across files (`build/amdcs pack|unpack|list|bench`).
  It helps on sets of similar files and does nothing for unrelated or random data, and it does not beat `tar+zstd` on ratio. Details, honest numbers and limitations: `docs/AMDC-S_Solid_Mode.md`.

## Results (complete Silesia corpus, 211.9 MB, one core, every round trip verified)

| tool | size | compress MB/s | decompress MB/s |
|---|---|---|---|
| gzip -9 | 31.91% | 8.8 | 136 |
| zstd -22 | 24.78% | 1.4 | 353 |
| brotli -11 | 23.39% | 0.40 | 218 |
| xz -9e | 22.86% | 1.3 | 62 |
| zpaq -m3 | 21.96% | 3.3 | 4.0 |
| **amdc1 -l 0** | 21.28% | 0.92 | 0.80 |
| **amdc1 -l 1** | **19.01%** | 0.26 | 0.28 |
| zpaq -m5 | **18.45%** | 0.35 | 0.35 |

amdc1 -l 1 is smaller than every mainstream tool (gzip, bzip2, xz, 7z, zstd, brotli) on all 12 files: 16.9% smaller than xz, 18.7% than brotli, 23.3% than zstd.
**zpaq -m5 is still 3.0% smaller overall** (amdc1 wins `nci`, `sao`, `xml`). Context mixing decodes as slowly as it encodes
(about 219-1,257x slower than xz/zstd decoding), so this is a ratio-first codec; use `amdcf` when decode speed matters. Full tables, memory
settings and caveats: spec section 12; raw data: `bench/results_silesia.csv`. Timings come from a slow 2.1 GHz virtualised core; compare ratios between rows, not absolute speeds.
