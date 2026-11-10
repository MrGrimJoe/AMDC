# AMDC-1 v1.1: Adaptive Multi-Model Data Compression

**Status:** implemented (C++17 reference + independent Python reference), tested, benchmarked on a small corpus.
**Objective:** exact lossless reconstruction, smallest measured archive.
**Lossy mode:** out of scope for this version (see section 10).

This revises the v1.0 research specification. Section 11 lists every change and why. Everything marked **normative** is
implemented identically in `src/` (C++) and `reference/amdc1/` (Python); the test suite requires the two to produce
byte-identical archives for the same input and settings.

## 1. Objective

For input bytes $X$ produce an archive $A$ minimizing $|A|$ subject to $\mathrm{Decode}(A)=X$. $|A|$ includes every header,
length, parameter and checksum. No lossless coder shortens every input, so each block may be stored raw.

## 2. Reversible transforms (normative)

All transforms are length-preserving and applied per block. Parameters are stored in the stream; the decoder never guesses.

| id | name | parameter | forward |
|---|---|---|---|
| 0 | identity | none | $y=x$ |
| 1 | delta | $k\in[1,255]$ | $y_t=x_t$ for $t<k$, else $(x_t-x_{t-k}) \bmod 256$ |
| 2 | xor | $k\in[1,255]$ | $y_t=x_t$ for $t<k$, else $x_t\oplus x_{t-k}$ |
| 3 | byte-plane shuffle | width $w\in[2,255]$ | with $m=n-(n \bmod w)$: plane $j$ = bytes $x_j,x_{j+w},\dots<m$, planes concatenated, then the $n \bmod w$ tail bytes unchanged |
| 4 | image | see section 8 | PNM residual planes |

(Indices are 0-based here; v1.0 used 1-based.)

## 3. Probability model (normative)

v1.0 mixed 11 byte-wise (256-ary) experts with Bayesian weights. v1.1 keeps the idea (many causal experts, adaptive
combination) but decomposes each byte into 8 binary decisions and mixes in the logistic domain, the approach used by the
PAQ/lpaq family. It is far cheaper per symbol and, in practice, stronger. The cost is the clean $\log_2 K$ regret bound of
the Bayesian mixture, which does **not** carry over (section 4).

**Arithmetic.** Integers only: no floating point anywhere in coding. Python integers are unbounded and C++ uses `int64`/
`uint64` with explicit wrap only where noted; `>>` on negatives is arithmetic (floor).

**Tables.** `squash(d)` for $d\in[-2047,2047]$ is the 33-point interpolated table
`T = 1,2,3,6,10,16,27,45,73,120,194,310,488,747,1101,1546,2047,2549,2994,3348,3607,3785,3901,3975,4022,4050,4068,4079,4085,4089,4092,4093,4094`:
$w=d \,\&\, 127,\; i=(d\gg7)+16,\; \mathrm{squash}=(T_i(128-w)+T_{i+1}w+64)\gg7$; saturating to 4095 / 0 outside the range.
`stretch` is its inverse table over 12-bit probabilities (`stretch[p]` = smallest $d$ with squash$(d)\ge p$).
`recip[n] = floor(131072 / (2n+3))`, approximately $1/(n+1.5)$ in 16-bit fixed point.

**Context models.** Eight hashed models, each with a 16-bit probability table `P` (init 32768) and an 8-bit count table `C`
(init 0), both of size $2^{tb}$, $tb=\min(22,\max(16,\mathrm{bitlen}(8n)+1))$. With $c_1,c_2,\dots$ the previous bytes,
$c_8$ the last 8 bytes as a 64-bit integer, and stride $s$ (a block parameter):

| model | context | count limit |
|---|---|---|
| 0 | none (order 0) | 250 |
| 1 | $c_1$ | 250 |
| 2 | $c_1c_2$ | 250 |
| 3 | $c_1c_2c_3$ | 120 |
| 4 | order 4 | 60 |
| 5 | order 6 | 60 |
| 6 | $s>0$: $(x_{t-s}, c_1)$; $s=0$: (rolling word hash, $c_1$) | 120 |
| 7 | $s>0$: $(x_{t-s}, x_{t-2s})$; $s=0$: $(c_2c_3)$ (sparse) | 120 |

(Out-of-range history reads as 0. Word hash: `wh = wh*0x2F0B4A27 + byte + 1 mod 2^32` over runs of letters, digits and bytes ≥128; reset to 0 otherwise.)
Slot base for model $i$ with context value $v$: $\big((v+1+i\cdot 0x10001)\cdot \mathrm{0x9E3779B97F4A7C15} \bmod 2^{64}\big)\gg(64-tb)$;
per-bit index: $(\mathrm{base}+c_0\cdot \mathrm{0x9E3779B1}) \,\&\, (2^{tb}-1)$, where $c_0$ is the partial byte with a leading 1.
Collisions are not detected (a known limitation).

**Counter update** after coding bit $b$: `P += (((b<<16) - P) * recip[C]) >> 16`; `C` increments up to the limit.
Model input to the mixer is `stretch[P>>4]`.

**Match model.** Table of $2^{mb}$ positions, $mb=\min(22,\max(12,\mathrm{bitlen}(n)+1))$, keyed by the hash of the last 5 bytes
($(c_8 \,\&\, 2^{40}-1)\cdot$`0x9E37...15` $\gg (64-mb)$). At each byte boundary: extend the current match if the predicted byte was right, else drop it; if
none, look up the candidate and verify backwards up to 32 bytes, accepting length $\ge5$; then store the current position.
While the predicted byte agrees with the bits coded so far, it contributes `±stretch[MP[len bucket]>>4]` (sign = predicted bit);
bucket $=\mathrm{len}$ for $<16$, else $\min(16+((\mathrm{len}-16)\gg2),32)$. `MP` (init 49152) updates `+= ((hit<<16)-MP) >> 6`.

**Mixer.** Inputs: 8 model stretches, the match input, and a constant bias 256 (10 total). One weight vector per
$(c_0, \text{match-active})$ pair (512 sets), initial weights 13107 for the nine non-bias inputs and 0 for the bias (65536 = 1.0).
$p=\mathrm{squash}(\mathrm{clamp}_{\pm2047}((\sum w_ix_i)\gg16))$, clamped to $\ge1$. After the bit: $e=((b\ll12)-p)\cdot 12$, $w_i \mathrel{+}= (x_ie)\gg14$
(i.e. learning rate $\eta = 12/1024$, tuned on text/JSON/float data).

## 4. What is and is not guaranteed

* **Guaranteed:** exact round-trip (checked by CRC-32 and by tests); no expansion beyond framing: a block is stored raw
  whenever coding it is not smaller.
* **Not guaranteed:** that v1.1 beats any other compressor. Results are measured, never assumed (section 12).
* The v1.0 bound $-\log_2 P_{\mathrm{mix}}(Y)\le -\log_2 P_j(Y)+\log_2 K$ holds only for the **Bayesian uniform mixture**. The
  logistic mixer has no such worst-case bound; its justification is empirical. The Bayesian formulation is retained as a
  possible future "AMDC-B" mode, not implemented.

## 5. Binary arithmetic coder (normative)

32-bit carry-less coder, 12-bit probabilities ($p=P(\text{bit}=1)\in[1,4094]$). State $[x_1,x_2]$ starts at $[0,2^{32}-1]$.
$x_{mid}=x_1+((x_2-x_1)\gg12)\cdot p$; bit 1 sets $x_2=x_{mid}$, bit 0 sets $x_1=x_{mid}+1$. While the top bytes of $x_1,x_2$
agree, shift one byte out ($x_1\ll8$, $x_2\ll8\,|\,255$). Flush writes $x_1$ as 4 big-endian bytes. The decoder preloads 4
bytes (missing bytes read as 0) and decodes bit $=[x\le x_{mid}]$.
Replaces v1.0 sections 5 and 6 (the 256-ary $M=2^{16}$ quantizer and range coder), which needed a per-symbol fractional-part
sort and were far slower. Limitation: probability resolution is 1/4096, so with $p\in[1,4094]$ a 0 bit never costs less than about 0.00035 bits (a 1 bit about 0.0007).
Highly redundant data therefore has a floor of roughly 1 byte per 2800 input bytes (1 MiB of zeros encodes to 397 bytes; see the `zeros` benchmark row).

## 6. Block and transform selection

Blocks (default 1 MiB) are independent: each starts with fresh model state, which allows parallel encode and decode at a small
learning cost. For each block the encoder chooses among 10 candidates $(\text{transform}, k, s)$:
`(id,-,0) (id,-,2) (id,-,4) (delta,1,1) (delta,2,2) (delta,4,4) (xor,1,1) (xor,4,4) (shuffle,2,0) (shuffle,4,0)`
plus raw. $J_i = 8\cdot|\text{record}_i|$ uses the real encoded record length (headers included); ties go to the earlier candidate.
Levels: **0** fixed `(id,-,0)`; **1** (default) score every candidate on a 12 KiB sample (begin, middle, end thirds), then fully
encode the winner (raw if the sample is incompressible); **2** fully encode all candidates. Level 1 is a heuristic: it can
miss the best candidate. Encoder search policy does not affect decodability.

## 7. Container (normative)

```
"AMDC" | version u8 = 1 | orig_len | block_size | block records | CRC-32 of original (u32 little-endian)
record:  0 | raw_len | bytes                                   (raw)
         1 | raw_len | xf u8 | params | s | payload_len | payload   (context-mixed)
params:  xf 1,2,3 -> k ; xf 4 -> hdr_len, w, h, channels, colour_transform ; xf 0 -> none
```
All integers are unsigned LEB128 varints (at most 9 bytes). Decoder requirements, all enforced and tested:
reject bad magic/version, truncation, trailing bytes, `orig_len > max_output` (default 4 GiB, configurable), block size outside
$[1,2^{28}]$, `raw_len` outside $[1,\text{block\_size}]$ or overrunning `orig_len`, stride $>2^{24}$, invalid transform parameters,
and any CM block with `raw_len > 4096 × (payload_len + 8)` (the measured maximum is about 2650:1, for all-zero data). All records are validated
**before** any output memory is allocated or any block decoded. A CRC mismatch is an error. Note: the block-size field is an
upper bound and the coder's flush tail has don't-care bytes, so a few single-byte flips still decode to the *correct*
output; the guarantee is "never silently wrong", which the tests assert.

## 8. Lossless image path (PGM/PPM, 8-bit)

Applies to binary `P5`/`P6` files with maxval 255. The whole file is one block (`xf 4`); header bytes and any trailing
bytes pass through verbatim, so reconstruction is **byte-exact**, not merely pixel-exact.

* Colour transform (`ct`): 0 none, or 1 "subtract green": $(R-G)\bmod256,\ G,\ (B-G)\bmod256$ (RGB only). The encoder tries both at level ≥ 1.
* Prediction per plane, raster order: MED $\hat I$ as in v1.0 (first row: left neighbour; first column: neighbour above; pixel (0,0): 0).
* Residual $e=(I-\hat I)\bmod256$, folded to a byte: $Z=2e$ for $e<128$, else $511-2e$ (a bijection on 0..255).
* Layout: header, plane 0 residuals, plane 1, plane 2, tail; coded with context stride $s=\text{width}$, so models 6 and 7 see the residual above and two rows above.

**Deviation from v1.0:** YCoCg-R makes Co and Cg 9-bit, which does not fit a byte alphabet. A 16-bit-symbol YCoCg-R path is a candidate for format v2.
The encoder always compares the image path against ordinary block coding and keeps the smaller archive.

## 9. Implementation map

| spec | C++ | Python reference |
|---|---|---|
| 3, 5 | `src/cm.cpp` | `reference/amdc1/cm.py` |
| 2, 8 | `src/transforms.cpp` | `reference/amdc1/transforms.py`, `image.py` |
| 6, 7 | `src/container.cpp` | `reference/amdc1/container.py` |

## 10. Perceptual (lossy) mode

Unchanged from v1.0 intent and still unimplemented: a separate format mode, never presented as lossless, with a
rate-distortion objective $J=R+\lambda D$ whose distortion measure must be validated against human evaluation.

## 11. Changes from v1.0

1. **Document repaired.** v1.0's Markdown contained pandoc artifacts (`{=tex}`, broken `\\ldots` escapes) that made the maths unreadable.
2. **Model:** 256-ary Bayesian mixing replaced by bitwise logistic mixing (section 3); regret bound no longer applies (section 4).
3. **Entropy coder:** 256-ary range coder + largest-remainder quantization replaced by a binary arithmetic coder (section 5).
4. **Underspecified items made concrete:** exact contexts, hashes, tables, learning rate, count limits, match model, container, byte order, integer encoding, maximum sizes, checksum, malformed-input behaviour, and fixed candidate list.
5. **Decoder hardening:** expansion-ratio guard, validate-then-allocate, bounded varints.
6. **Image path:** modular subtract-green + MED in place of YCoCg-R (section 8); byte-exact file reconstruction.
7. **Parallelism:** independent blocks; output identical for any thread count.
8. **Dropped from the v1.0 expert list:** the separate residual (delta/XOR) *experts* and repeated-byte expert. Delta and XOR remain as selectable *transforms*, and the match model plus order-1 context subsume the repeated-byte predictor. Re-adding them as ablation studies is future work.

## 12. Benchmark protocol and results

Protocol as in v1.0: total archive size with headers, throughput, exact round-trip, per-file and aggregate results, competitors at strong settings.
Run `python3 bench/bench.py [DIR ...]`. Measured on the bundled synthetic + small-file corpus (1.85 MB, single core, one machine):

| | gzip -9 | bzip2 -9 | xz -9e | AMDC-1 L1 |
|---|---|---|---|---|
| total bytes | 600,635 | 630,534 | 457,920 | **366,463** |
| ratio | 32.5% | 34.2% | 24.8% | **19.9%** |
| time | 0.17 s | 0.19 s | 0.93 s | 3.0 s |

AMDC-1 wins on 9 of 10 files, including the structured numeric and image files, where the transforms and strided contexts help.
It loses on `zeros` (204 B vs bzip2's 48 B; section 5 floor) and is within noise of the others on `random` (+19 B framing).
**This corpus is small and partly synthetic; it demonstrates the architecture works, not that it beats mature compressors in general.**
Not yet done: zstd/brotli baselines (not installed here), Silesia/Canterbury-scale corpora, peak-RAM measurement, real photographs, and an ablation of each model.
Decoding is symmetric with encoding (about 0.7 MB/s single core for the model itself), so AMDC-1 is an archival codec, not a fast one.
