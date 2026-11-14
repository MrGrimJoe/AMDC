# AMDC-B: BWT tier (experimental)

A third codec between AMDC-F (LZ, hundreds of MB/s, LZ4-class ratio) and AMDC-1 (context mixing, under 1 MB/s). It is a classic Burrows-Wheeler
pipeline with a modern coder: **SA-IS suffix sorting -> BWT -> move-to-front ranks -> context-mixed binary coding of each rank**. Programs: `build/amdcb`.
Source: `src/bwt.*` (transform), `src/bwtcodec.*` (model, container), `src/amdcb_main.cpp` (CLI), `tests/test_bwt.cpp`.

**Status: experimental.** It round-trips (3,365 checks, also clean under ASan/UBSan, including every-length truncation and 3,000 mutations) but there is **no independent
reference implementation** (unlike AMDC-1 and AMDC-F) and the format may still change. It does **not** reach the 10-100 MB/s target on one core (see the numbers below).

## Design

* Blocks (default 8 MiB, `-b`) are independent and run in parallel (`-j`); the archive is identical for any thread count. Each block stores a 64-bit checksum of the original, verified on decode.
* Forward: SA-IS over the block plus an end marker (16-bit symbols so the marker is unique). Inverse: **four interleaved chains** (each starts at a stored row) with row index and symbol packed in one 32-bit cell, so four cache misses are in flight instead of one.
* Rank coder: for every rank a zero flag, then an Elias-gamma length in unary, two modelled mantissa bits and raw remaining bits. Each modelled decision mixes four adaptive contexts
  (previous two ranks, previous symbol, previous two symbols, zero-run length x previous rank) through a logistic mixer selected by (decision, previous rank), then an APM. Counters and tables are laid out node-major-last so one symbol's decisions share a cache line.
* Container: `"AMDB" | version | total | block size | blocks` where a block is `type | length | checksum | [primary row, 3 chain starts] | payload length | payload`. Incompressible blocks are stored raw. The decoder checks every length, refuses implausible expansion ratios and rejects trailing data.

## What was tried (all measured on the 12 Silesia 1.5 MiB slices, rank coder alone)

| change | effect |
|---|---|
| node-major-last table layout (cache locality) | same size, 12% faster |
| mixer learning rate 16x lower | **-1.1%** (kept) |
| adaptive-rate (count based) counters | -0.05% (not kept) |
| extra contexts: previous two symbols in MTF order, activity (sum of last four ranks), order-3 hash, symbol x rank pair | each between +0.02% and +0.15% (worse; not kept) |
| separate MTF list per previous symbol / per previous two symbols | **+4% / +8% worse** (not kept) |

Where the bits go (after tuning the rate): zero flag 14%, gamma length 34%, first mantissa bit 12% (0.95 bits each), second mantissa bit 11% (0.98 bits each),
**raw mantissa bits 28% (1.00 bit each)**. In other words large MTF ranks are close to random, because the transform discards symbol identity that the contexts could have used.
That, not the counters or contexts, is what caps this design near 32-33% on these slices; getting meaningfully below needs a symbol-domain model for escaped (large-rank) symbols, which
costs speed. That is the next experiment, not part of this patch.
