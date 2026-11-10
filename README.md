# AMDC-1: Adaptive Multi-Model Data Compression

Lossless compressor: reversible transforms + 8 hashed context models + match model, mixed bitwise with a
logistic mixer and coded with a binary arithmetic coder. C++17, no dependencies. Spec: `docs/AMDC-1_Specification_v1.1.md`.

```
make                      # builds build/amdc1 and build/test_amdc1
make test                 # 142 unit checks + C++/Python byte-identity cross-check
build/amdc1 compress   [-b BYTES] [-l 0|1|2] [-j N] [--verify] IN OUT
build/amdc1 decompress [--max-output BYTES] [-j N] IN OUT
python3 bench/bench.py [DIR ...]   # vs gzip / bzip2 / xz (/ zstd / brotli if installed)
```

* `-l 0` fixed model (fast), `-l 1` sampled transform search (default), `-l 2` exhaustive search.
* Blocks (default 1 MiB) are independent and processed in parallel; output is identical for any `-j`.
* 8-bit PGM/PPM files get a dedicated lossless image path and are reproduced byte-for-byte.
* `reference/amdc1/` is an independent pure-Python implementation of the same format (`PYTHONPATH=reference python3 -m amdc1 ...`).

Speed is roughly 0.7 MB/s per core in both directions: this is an archival-ratio codec, not a fast one.
Treat benchmark numbers as preliminary (see spec section 12). Also verified under ASan/UBSan, plus a 3000-case mutation fuzz of the decoder.
