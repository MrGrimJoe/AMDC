# AMDC-1: Adaptive Multi-Model Data Compression

Two lossless codecs in one repo.

* **`amdc1`: maximum ratio** (context mixing), ~0.5 MB/s. Spec: `docs/AMDC-1_Specification_v1.2.md`.
* **`amdcf`: maximum decode speed** (LZ4-compatible LZ77), ~2-4 GB/s per core, with a RAM page codec and compressed page cache. See `docs/AMDC-F_Fast_Mode.md` (including what it cannot do for Windows).

`amdc1` details: lossless compressor: reversible transforms + 10 hashed context models + match model, mixed bitwise by two logistic
mixers, refined by an APM stage and coded with a 16-bit-probability binary arithmetic coder. C++17, no dependencies. Spec: `docs/AMDC-1_Specification_v1.2.md`.

```
make                      # builds amdc1, amdcf and both test programs
make test                 # 146 + 1378 unit checks, C++/Python byte-identity cross-check
make sanitize             # fast codec under ASan/UBSan, incl. 200k-mutation decoder fuzz
make windows              # amdcf.exe / amdc1.exe / test_fast.exe via MinGW-w64
build/amdc1 compress   [-b BYTES] [-l 0|1|2] [-j N] [--verify] IN OUT
build/amdc1 decompress [--max-output BYTES] [-j N] IN OUT
python3 bench/bench.py [DIR ...]   # vs gzip / bzip2 / xz / zstd / brotli (zstd, brotli via `pip install zstandard brotli`)
tools/ablate.sh DIR                # size impact of removing each model component
```

* `-l 0` fixed model (fast), `-l 1` sampled transform search (default), `-l 2` exhaustive search.
* Blocks (default 1 MiB) are independent and processed in parallel; output is identical for any `-j`.
* 8-bit PGM/PPM files get a dedicated lossless image path and are reproduced byte-for-byte.
* `reference/amdc1/` is an independent pure-Python implementation of the same format (`PYTHONPATH=reference python3 -m amdc1 ...`).

Results on 13 held-out files (2.08 MB): 13.70% of original vs 15.58% for xz -9e and 15.73% for brotli-11; smallest on every file.
Speed is about 0.45-0.76 MB/s per core and memory about 180 MiB per 1 MiB block: an archival-ratio codec, not a fast one.
Treat results as preliminary (spec section 12). Verified under ASan/UBSan plus a 3000-case mutation fuzz of the decoder.
