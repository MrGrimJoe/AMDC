# AMDC-S: optional solid multi-file mode (experimental)

AMDC-F compresses every file, and every 256 KiB block, independently, and its LZ4-format blocks have a 64 KiB match window.
AMDC-S is a **separate, optional archive format** that lets that same block compressor find repeated data *between* files.
It does not change `block_compress`, `block_decompress`, the AMDF container, the page codec or any existing API.

## How it works

* Files are put in an order (`-o name|ext|size|extsize`) and cut into **groups of G files** (`-g`).
* Inside a group, each file is split into **units of U pages** (`-u`, 4 KiB pages; `-u 0` = whole files back to back, i.e. plain solid).
  Round 0 takes unit 0 of every file, round 1 takes unit 1 of every file, and so on. Files that run out simply drop out of later rounds;
  a final partial page keeps its exact length; empty files have no data and exist only in the metadata.
* That "virtual stream" is cut into **independent stripes** (`-s`, default 256 KiB) and each stripe goes through the unchanged
  AMDC-F block compressor (level 1 or 2). Stripes are independent, so compression and decompression are parallel and the output is
  byte-identical for any thread count.
* Metadata (paths, sizes, a 64-bit hash per file) is stored LZ4-compressed; a stripe directory records each stripe's type
  (raw / LZ4 block / fill) and length; an 8-byte trailer hashes the whole archive.

Format and API: `src/solid.hpp`. CLI: `build/amdcs pack|unpack|list|bench` (see `amdcs` with no arguments).
Build: `make` (adds `build/amdcs` and `build/test_solid`; `make test` and `make sanitize` run the solid tests).

## Safety

* `unpack` checks the trailer hash, header limits, metadata length/ratio, path rules, duplicate paths, file-versus-directory conflicts,
  declared sizes against `--max-output`, per-stripe ratio plausibility, exact stripe-data length and every file's hash.
  Anything else is rejected with an error, never partially trusted. Corruption fuzzing (9,000 mutated archives per run) is part of `make test`.
* Paths must be relative, `/`-separated, with no empty, `.` or `..` components, no `\`, `:`, NUL or control characters.
* `extract_to` re-validates every path, refuses symlinked or non-directory path components, creates files with `O_EXCL|O_NOFOLLOW`
  (POSIX), and refuses to overwrite existing files unless `-f` (and even then never replaces a symlink or non-regular file).
  Tests cover a symlinked directory pointing outside the target, a symlinked final component, and `../` / absolute paths.
* Not covered: races where something else modifies the destination while extracting; the Windows branch of `extract_to`
  (plain `fopen`, no symlink hardening beyond the pre-checks) was not built or tested here.

## Known limitations

* **Memory:** pack and unpack hold the whole dataset in RAM (peak about 3.5x raw size to pack, 1.7x to unpack in the runs below).
* **No file modes or timestamps** are stored (executable bits are lost); `tar` stores them, which costs it about 512 bytes per file.
* The match window is still 64 KiB, so cross-file sharing only works for data that ends up within 64 KiB in the virtual stream.

## Results

Machine: shared 2-vCPU Xeon 2.1 GHz VM, so speeds carry roughly +/-30% noise; **sizes are exact and deterministic**.
Datasets are copies of files already on this machine, size-capped (`bench/make_datasets.py`): Python stdlib `.py` files, three Python
versions side by side (`source_versions`), `/usr/include`, `/usr/bin`, shared libraries, `/usr/share/doc`, a mixed sample of `/usr`, and
300 random 100 KiB files as a negative control. Every configuration was round-tripped to disk and diffed against the original tree
(`diff -rq`), zero mismatches. Baselines: "AMDC-F indep" = the existing library call `amdcf compress` makes, once per file (sizes are
summed over the per-file outputs); `tar+lz4` = python-lz4 frame format, `tar+zstd` = python-zstandard, both on a `tar` stream
(sorted, owner and mtime zeroed) in single-shot in-memory mode, so they use one thread while AMDC-F/AMDC-S use both cores.
AMDC-S "concat" is `-g 1024 -u 0 -o ext`; "interleave" is `-g 32 -u 1 -o extsize` (chosen from the sweep in `bench/results_solid/explore*.tsv`;
it was not tuned per dataset). Scripts: `bench/bench_inmem.py`, `bench/bench_solid.py`, `bench/report.py`; raw data in `bench/results_solid/`.

**Archive size as % of raw bytes (metadata included; independent = sum of per-file outputs)**

| dataset (raw MB) | AMDC-F indep L1 | AMDC-F indep L2 | AMDC-S concat L1 | AMDC-S concat L2 | AMDC-S interleave L1 | AMDC-S interleave L2 | tar+lz4 level 0 | tar+lz4 level 9 | tar+zstd level 1 | tar+zstd level 3 | tar+zstd level 19 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| binaries (50.3) | 55.9 | 49.2 | 55.5 | 48.7 | 57.1 | 50.8 | 58.0 | 46.9 | 44.8 | 39.5 | 31.2 |
| libraries (50.3) | 52.2 | 45.9 | 52.2 | 45.9 | 54.3 | 48.3 | 54.2 | 44.4 | 41.8 | 37.5 | 30.0 |
| headers (42.9) | 37.1 | 31.4 | 30.3 | 23.3 | 34.9 | 28.5 | 30.0 | 22.1 | 20.2 | 17.7 | 12.9 |
| source_py (10.6) | 41.2 | 33.8 | 37.2 | 28.4 | 39.8 | 31.9 | 36.8 | 26.5 | 24.9 | 22.7 | 16.9 |
| source_versions (32.8) | 41.7 | 34.2 | 37.5 | 28.6 | 25.2 | 20.4 | 37.2 | 26.7 | 25.1 | 22.8 | 16.7 |
| text_docs (19.0) | 36.2 | 30.9 | 24.4 | 18.8 | 15.0 | 12.3 | 24.6 | 17.2 | 13.8 | 11.4 | 5.0 |
| mixed (50.3) | 54.5 | 50.0 | 53.1 | 48.3 | 53.9 | 49.3 | 53.8 | 47.4 | 44.6 | 42.3 | 38.0 |
| random_control (30.7) | 100.0 | 100.0 | 100.0 | 100.0 | 100.0 | 100.0 | 100.4 | 100.3 | 100.5 | 100.5 | 100.3 |

**Codec-only compression throughput, MB/s of raw data (in memory, no disk I/O)**

| dataset (raw MB) | AMDC-F indep L1 | AMDC-F indep L2 | AMDC-S concat L1 | AMDC-S concat L2 | AMDC-S interleave L1 | AMDC-S interleave L2 | tar+lz4 level 0 | tar+lz4 level 9 | tar+zstd level 1 | tar+zstd level 3 | tar+zstd level 19 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| binaries (50.3) | 353 | 74 | 341 | 82 | 340 | 86 | 433 | 29 | 440 | 184 | 2 |
| libraries (50.3) | 395 | 82 | 372 | 89 | 370 | 82 | 458 | 29 | 415 | 194 | 2 |
| headers (42.9) | 381 | 86 | 553 | 118 | 463 | 114 | 519 | 37 | 439 | 291 | 2 |
| source_py (10.6) | 340 | 62 | 496 | 94 | 482 | 96 | 421 | 24 | 371 | 214 | 2 |
| source_versions (32.8) | 351 | 61 | 462 | 93 | 673 | 136 | 360 | 24 | 367 | 164 | 2 |
| text_docs (19.0) | 361 | 83 | 701 | 147 | 872 | 240 | 625 | 56 | 439 | 408 | 5 |
| mixed (50.3) | 393 | 84 | 534 | 96 | 435 | 100 | 614 | 33 | 469 | 218 | 3 |
| random_control (30.7) | 1877 | 75 | 993 | 102 | 894 | 113 | 1494 | 38 | 2041 | 1603 | 4 |

**Codec-only decompression throughput, MB/s of raw data (in memory, no disk I/O)**

| dataset (raw MB) | AMDC-F indep L1 | AMDC-F indep L2 | AMDC-S concat L1 | AMDC-S concat L2 | AMDC-S interleave L1 | AMDC-S interleave L2 | tar+lz4 level 0 | tar+lz4 level 9 | tar+zstd level 1 | tar+zstd level 3 | tar+zstd level 19 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| binaries (50.3) | 1295 | 1378 | 1343 | 1731 | 1518 | 1582 | 1644 | 1869 | 1053 | 866 | 579 |
| libraries (50.3) | 1426 | 1378 | 1502 | 1650 | 1530 | 1508 | 1578 | 1682 | 871 | 858 | 512 |
| headers (42.9) | 1053 | 1036 | 1509 | 1644 | 1335 | 1373 | 1700 | 1861 | 958 | 1294 | 1055 |
| source_py (10.6) | 1095 | 1184 | 1432 | 1513 | 1401 | 1452 | 1662 | 1844 | 1234 | 1023 | 835 |
| source_versions (32.8) | 1168 | 1167 | 1220 | 1719 | 1677 | 1809 | 1614 | 1711 | 935 | 731 | 773 |
| text_docs (19.0) | 1033 | 967 | 1591 | 1772 | 1915 | 2003 | 2014 | 1778 | 1421 | 1502 | 2491 |
| mixed (50.3) | 1021 | 1298 | 1673 | 1662 | 1451 | 1415 | 1832 | 2005 | 1082 | 821 | 828 |
| random_control (30.7) | 3162 | 3230 | 1566 | 1981 | 1905 | 1715 | 3220 | 3113 | 6451 | 7033 | 5795 |

**End-to-end pack from directory: wall seconds (best of 3, includes reading files and writing the archive)**

| dataset (raw MB) | AMDC-F indep L1 | AMDC-F indep L2 | AMDC-S concat L1 | AMDC-S concat L2 | AMDC-S interleave L1 | AMDC-S interleave L2 | tar+lz4 level 0 | tar+lz4 level 9 | tar+zstd level 1 | tar+zstd level 3 | tar+zstd level 19 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| binaries (50.3) | 0.31 | 0.85 | 0.37 | 0.85 | 0.33 | 0.82 | 0.21 | 2.09 | 0.23 | 0.37 | 27.51 |
| libraries (50.3) | 0.27 | 0.81 | 0.32 | 0.80 | 0.32 | 0.80 | 0.22 | 2.04 | 0.24 | 0.42 | 26.75 |
| headers (42.9) | 0.43 | 1.29 | 0.29 | 0.55 | 0.32 | 0.60 | 0.21 | 1.12 | 0.20 | 0.32 | 22.83 |
| source_py (10.6) | 0.35 | 0.51 | 0.07 | 0.15 | 0.08 | 0.17 | 0.06 | 0.46 | 0.07 | 0.11 | 6.31 |
| source_versions (32.8) | 1.07 | 1.45 | 0.20 | 0.46 | 0.16 | 0.37 | 0.15 | 1.34 | 0.17 | 0.22 | 19.91 |
| text_docs (19.0) | 0.79 | 1.01 | 0.11 | 0.22 | 0.11 | 0.17 | 0.10 | 0.39 | 0.12 | 0.10 | 3.77 |
| mixed (50.3) | 1.51 | 2.06 | 0.33 | 0.73 | 0.33 | 0.69 | 0.21 | 1.50 | 0.22 | 0.33 | 19.34 |
| random_control (30.7) | 0.21 | 0.63 | 0.15 | 0.45 | 0.15 | 0.40 | 0.11 | 0.84 | 0.10 | 0.12 | 8.43 |

**End-to-end unpack to directory: wall seconds (best of 2, includes writing every file)**

| dataset (raw MB) | AMDC-F indep L1 | AMDC-F indep L2 | AMDC-S concat L1 | AMDC-S concat L2 | AMDC-S interleave L1 | AMDC-S interleave L2 | tar+lz4 level 0 | tar+lz4 level 9 | tar+zstd level 1 | tar+zstd level 3 | tar+zstd level 19 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| binaries (50.3) | 0.17 | 0.16 | 0.22 | 0.22 | 0.21 | 0.21 | 0.18 | 0.17 | 0.19 | 0.20 | 0.23 |
| libraries (50.3) | 0.17 | 0.13 | 0.16 | 0.18 | 0.19 | 0.15 | 0.21 | 0.21 | 0.21 | 0.24 | 0.28 |
| headers (42.9) | 0.26 | 0.50 | 1.29 | 1.91 | 1.84 | 1.59 | 1.92 | 1.76 | 2.23 | 1.80 | 1.80 |
| source_py (10.6) | 0.29 | 0.28 | 0.27 | 0.28 | 0.28 | 0.30 | 0.36 | 0.32 | 0.34 | 0.40 | 0.25 |
| source_versions (32.8) | 0.92 | 0.88 | 0.72 | 0.69 | 0.77 | 0.81 | 0.85 | 0.89 | 0.87 | 0.79 | 1.05 |
| text_docs (19.0) | 0.70 | 0.76 | 0.79 | 0.86 | 0.85 | 0.78 | 0.77 | 0.89 | 0.90 | 0.91 | 0.72 |
| mixed (50.3) | 0.85 | 0.99 | 1.40 | 1.27 | 1.28 | 1.57 | 1.42 | 1.46 | 1.39 | 1.12 | 1.63 |
| random_control (30.7) | 0.20 | 0.24 | 0.26 | 0.26 | 0.27 | 0.15 | 0.25 | 0.29 | 0.26 | 0.22 | 0.27 |

**Pack CPU seconds (user+sys, whole process tree)**

| dataset (raw MB) | AMDC-F indep L1 | AMDC-F indep L2 | AMDC-S concat L1 | AMDC-S concat L2 | AMDC-S interleave L1 | AMDC-S interleave L2 | tar+lz4 level 0 | tar+lz4 level 9 | tar+zstd level 1 | tar+zstd level 3 | tar+zstd level 19 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| binaries (50.3) | 0.41 | 1.39 | 0.48 | 1.47 | 0.44 | 1.40 | 0.24 | 2.12 | 0.28 | 0.41 | 27.26 |
| libraries (50.3) | 0.36 | 1.34 | 0.41 | 1.36 | 0.42 | 1.35 | 0.26 | 2.06 | 0.28 | 0.46 | 26.58 |
| headers (42.9) | 0.43 | 1.29 | 0.34 | 0.88 | 0.39 | 0.94 | 0.25 | 1.15 | 0.24 | 0.36 | 22.67 |
| source_py (10.6) | 0.35 | 0.52 | 0.09 | 0.25 | 0.10 | 0.28 | 0.07 | 0.45 | 0.08 | 0.12 | 6.31 |
| source_versions (32.8) | 1.08 | 1.47 | 0.25 | 0.77 | 0.20 | 0.57 | 0.18 | 1.35 | 0.19 | 0.25 | 19.73 |
| text_docs (19.0) | 0.79 | 1.04 | 0.13 | 0.33 | 0.13 | 0.24 | 0.12 | 0.41 | 0.13 | 0.12 | 3.72 |
| mixed (50.3) | 1.54 | 2.32 | 0.40 | 1.20 | 0.39 | 1.11 | 0.26 | 1.55 | 0.26 | 0.38 | 19.28 |
| random_control (30.7) | 0.20 | 0.62 | 0.16 | 0.71 | 0.15 | 0.65 | 0.14 | 0.86 | 0.13 | 0.14 | 8.34 |

**Pack peak memory, MB (largest process in the tree)**

| dataset (raw MB) | AMDC-F indep L1 | AMDC-F indep L2 | AMDC-S concat L1 | AMDC-S concat L2 | AMDC-S interleave L1 | AMDC-S interleave L2 | tar+lz4 level 0 | tar+lz4 level 9 | tar+zstd level 1 | tar+zstd level 3 | tar+zstd level 19 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| binaries (50.3) | 43 | 42 | 183 | 174 | 171 | 164 | 116 | 105 | 83 | 82 | 157 |
| libraries (50.3) | 30 | 26 | 163 | 159 | 175 | 169 | 112 | 103 | 82 | 81 | 157 |
| headers (42.9) | 15 | 15 | 128 | 122 | 138 | 138 | 81 | 74 | 66 | 66 | 144 |
| source_py (10.6) | 15 | 15 | 39 | 36 | 39 | 38 | 30 | 28 | 27 | 27 | 106 |
| source_versions (32.8) | 15 | 15 | 110 | 102 | 95 | 94 | 68 | 62 | 54 | 54 | 132 |
| text_docs (19.0) | 15 | 15 | 57 | 57 | 56 | 54 | 40 | 38 | 35 | 36 | 114 |
| mixed (50.3) | 30 | 31 | 168 | 163 | 189 | 180 | 113 | 107 | 85 | 85 | 162 |
| random_control (30.7) | 15 | 15 | 128 | 128 | 127 | 127 | 100 | 100 | 73 | 73 | 153 |

**Unpack peak memory, MB (largest process in the tree)**

| dataset (raw MB) | AMDC-F indep L1 | AMDC-F indep L2 | AMDC-S concat L1 | AMDC-S concat L2 | AMDC-S interleave L1 | AMDC-S interleave L2 | tar+lz4 level 0 | tar+lz4 level 9 | tar+zstd level 1 | tar+zstd level 3 | tar+zstd level 19 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| binaries (50.3) | 32 | 33 | 85 | 82 | 89 | 86 | 136 | 131 | 17 | 18 | 24 |
| libraries (50.3) | 21 | 21 | 81 | 78 | 84 | 81 | 134 | 129 | 17 | 18 | 24 |
| headers (42.9) | 15 | 15 | 61 | 58 | 63 | 61 | 113 | 109 | 16 | 18 | 24 |
| source_py (10.6) | 15 | 15 | 21 | 20 | 21 | 21 | 37 | 36 | 16 | 18 | 24 |
| source_versions (32.8) | 15 | 15 | 50 | 48 | 47 | 45 | 89 | 86 | 16 | 18 | 24 |
| text_docs (19.0) | 15 | 15 | 30 | 29 | 28 | 28 | 55 | 54 | 16 | 18 | 24 |
| mixed (50.3) | 18 | 18 | 81 | 79 | 82 | 80 | 138 | 134 | 17 | 18 | 24 |
| random_control (30.7) | 15 | 15 | 68 | 68 | 68 | 68 | 100 | 100 | 16 | 18 | 24 |


## What the numbers say

**Where solid mode helped (size, vs AMDC-F independent files at the same level):**
* `source_versions` (the same files from three Python versions): 41.7% -> 25.2% (L1), 34.2% -> 20.4% (L2). `text_docs`: 36.2% -> 15.0% (L1), 30.9% -> 12.3% (L2).
* `headers`, `source_py`: smaller by about 4-8 points, and plain solid ("concat") beats interleaving there (headers 30.3% vs 34.9% at L1).
* On the many-small-file sets pack time also drops a lot (e.g. `text_docs` 0.79 s -> 0.11 s end to end), mostly because per-file overhead disappears.

**Where it did not help:** `binaries`, `libraries` and `mixed` improved by 0-1.5 points with plain solid and got slightly worse with interleaving (about 1-2 points on
`binaries`/`libraries`); `random_control` is unchanged at 100.0%, as expected. Interleaving is not a general win: it only paid off on sets
with many near-identical files; it lost to plain concatenation on five of eight sets (tied on the random control).

**Interleaving versus ordering:** the gain on `source_versions` and `text_docs` is partly a *file-ordering* effect. Sorting similar files next
to each other also helps `tar`: `tar+zstd-3` on `source_versions` goes 22.8% -> 10.5% with the same extension/size order, and zstd-19 reaches 7.3%
(`bench/ordering_check.py`). Interleaving's real contribution is working around the 64 KiB window: on `source_versions` plain concat in that
order gives 32.1% (L1) / 24.7% (L2), interleaving 25.2% / 20.4%. Among LZ4-format, 64 KiB-window codecs it is a genuine improvement (it also beats
`tar+lz4-9` in the same order, 20.4% vs 22.6%, and 12.3% vs 14.6% on `text_docs`).

**It does not beat the mainstream tools on ratio.** zstd-3 is smaller than AMDC-S on every set, usually by a lot (headers 17.7% vs 28.5%, binaries 39.5% vs 50.8%),
except `source_versions` (zstd-3 22.8% vs AMDC-S L2 interleave 20.4%) and, for zstd-1, `text_docs` (13.8% vs 12.3%). Those are the cases where `tar` name order
separates similar files; in the same order as AMDC-S, zstd-3 gets 10.5% on `source_versions`. zstd-19 is smaller everywhere. `tar+lz4-9` is smaller than AMDC-S L2 on five of the seven compressible sets.

**Speed:** codec-only decompression of AMDC-S is about 1.2-2.0 GB/s, the same class as `tar+lz4` (1.6-2.0 GB/s) and faster than zstd on most of these
sets, but on binaries/libraries it is not faster than lz4. Compression at level 2 (about 80-240 MB/s) is slower than zstd-3 on every set and faster than lz4-9. Interleaving
adds gather/scatter copies; it was about as fast as concat in these runs. End-to-end extraction times are dominated by writing files and are
within noise of each other. Memory: AMDC-S needs far more RAM than the streaming alternatives (see tables).

## Decoder experiment (`block_decompress`, tight-output short-offset fallback)

Request: replace the final byte-at-a-time loop (tight output, offset 2-15) with `expand_period`. A guard was needed: `expand_period` assumes
`offset < mlen`, and for `mlen <= offset` it would write past the match; the experiment used a plain `memcpy` for that case.

| workload (`bench/decode_bench.cpp`, same flags, interleaved runs, 3 rounds, MB/s) | before | after |
|---|---:|---:|
| synthetic: blocks ending in a short-offset match, exact-size buffer | 1706 / 1709 / 1743 | 815 / 905 / 998 |
| 4 KiB pages, struct/pointer heap | 1191 / 1180 / 1215 | 1194 / 1261 / 1203 |
| 4 KiB pages, real process RAM dump | 2245 / 2044 / 2238 | 2171 / 2342 / 2149 |
| 4 KiB pages, real binaries | 2205 / 2470 / 2199 | 2193 / 2130 / 2308 |

* On the synthetic case it was **about 47% slower**; on real pages it was within noise. A second variant (memcpy only when the match does not overlap,
  byte loop otherwise) was about equal to the original (1590-1717 vs 1698-1822). **No variant was faster, so `src/fast.cpp` is unchanged.**
* Why it cannot matter much: instrumenting the decoder shows the fallback fires on only 0.00-0.04% of matches in real data
  (struct heap 724 of 1.93M, real RAM dump 1,349 of 4.48M, text 38 of 2.65M), i.e. only for matches that end within a few bytes of a block's end.
* The tests are kept: `test_short_period_tight` covers every offset 2-15 x every match length 4-300 x five tail lengths in exact-size heap buffers,
  rejection of off-by-one declared sizes and truncation; `test_short_period_mutation` runs 30,000 mutated blocks against the naive reference decoder.
  An unguarded `expand_period` fails them under ASan with a heap-buffer-overflow, so they do catch the bug.
* The struct-heap page speed (1.2 GB/s versus LZ4's 2.0 GB/s) is not explained by this fallback; the likelier cause is the general short-period path
  (a 16-iteration byte loop before the wide copy), which was not changed here.
