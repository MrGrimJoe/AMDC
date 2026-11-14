# AMDC-1 v1.4 (format version 4): Adaptive Multi-Model Data Compression

**Status:** implemented (C++17 + independent Python reference), tested, benchmarked on the Silesia corpus against common compressors.
**Objective:** exact lossless reconstruction, smallest measured archive.
**Lossy mode:** out of scope for this version (see section 10).

This revises the v1.0 research specification (format versions 1 to 3 are no longer readable). Section 11 lists every change and why. Everything marked **normative** is
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
| 5 | x86 | none | for each position $i$ scanned left to right (see below): $v=x_{i+1}|x_{i+2}\ll8|x_{i+3}\ll16$; $y_{i+1..i+3}$ = bytes of $(v+i+5) \bmod 2^{24}$ |

(Indices are 0-based here; v1.0 used 1-based.)

**x86 filter (id 5).** Scan $i=0,1,\dots$ while $i+4<n$, with $lf=-10$. If $x_i\in\{\mathrm{E8},\mathrm{E9}\}$, $i-lf>3$ and
$x_{i+4}\in\{\mathrm{00},\mathrm{FF}\}$: convert bytes $i+1..i+3$ (relative to absolute 24-bit, inverse subtracts) and continue at $i+5$.
Otherwise, if $x_i$ is E8/E9 set $lf=i$; continue at $i+1$. The trigger byte $x_{i+4}$ is never modified, and an examined-but-unconverted
opcode blocks conversion for the next three positions, so no later conversion can rewrite a trigger byte: forward and inverse make the same decisions.
(A first version without the $lf$ rule failed to round-trip when an unconverted opcode's trigger byte lay inside a later conversion window; the
regression is covered by tests.)

## 3. Probability model (normative)

The model decomposes each byte into 8 binary decisions and mixes many causal experts in the logistic domain (the PAQ/lpaq
family). Format version 4 adds **bit histories** (each slot also remembers a compact history of the bits seen in its context, mapped to a
probability by a learned state map), three more context models, a fourth mixer and a decaying mixer learning rate. A *lite* profile keeps the
fast v3 design. The clean $\log_2 K$ regret bound of a Bayesian mixture does **not** carry over (section 4).

**Arithmetic.** Integers only: no floating point anywhere in coding. Python integers are unbounded; C++ uses fixed-width
integers sized so that no intermediate overflows (verified by the bit-exact cross-check). `>>` on negatives is arithmetic (floor).

**Tables.** `squash(d)` for $d\in[-2047,2047]$ is the 33-point interpolated table
`T = 1,2,3,6,10,16,27,45,73,120,194,310,488,747,1101,1546,2047,2549,2994,3348,3607,3785,3901,3975,4022,4050,4068,4079,4085,4089,4092,4093,4094`:
$w=d \,\&\, 127,\; i=(d\gg7)+16,\; \mathrm{squash}=(T_i(128-w)+T_{i+1}w+64)\gg7$; saturating to 4095 / 0 outside the range.
`stretch` is its inverse table over 12-bit probabilities (`stretch[p]` = smallest $d$ with squash$(d)\ge p$).
`recip[n] = floor(131072 / (2n+3))`, approximately $1/(n+1.5)$ in 16-bit fixed point.

**Model configuration** (stored per block, section 7): `lite` (0 = full, 1 = lite) and `membits` $\in[16,24]$.

**Bit-history machine.** A state is $(n_0,n_1,\ell)$: bounded counts of zeros and ones seen in the context and the last bit $\ell$ (kept only when both counts are
non-zero, else 0). On observing bit $b$: $n_b \mathrel{+}= 1$; if the opposite count exceeds 2 it becomes $2+\lfloor(\text{opp}-2)/2\rfloor$; then with $m=\min(n_0,n_1)$ and
$\mathrm{lim}=(41,24,12,8,6,5)[m]$ (5 for $m\ge6$) both counts are clamped to $\mathrm{lim}$; $\ell=b$ if both counts are non-zero. State 0 is $(0,0,0)$. States are numbered
breadth first: process states in index order, for each take bit 0 then bit 1, and give each previously unseen result the next index. This yields 229 states, so a state fits in a byte.
`sn[q]` $=n_0+n_1$ and the initial state-map probability is `p0[q] = ((2n_1+1)*65535) // (2(n_0+n_1)+2)`.

**Context models.** With $c_1,c_2,\dots$ the previous bytes, $c_8$ the last 8 bytes as a 64-bit integer, stride $s$ (a block
parameter), $wh$ the hash of the current word, $pwh$, $ppwh$ the hashes of the previous two words, $ls$ and $pls$ the start offsets of the current and previous line
(a line starts after byte 10), and $x_{t-k}$ the byte $k$ positions back:

| model | context | count limit | history | in lite |
|---|---|---|---|---|
| 0 | none (order 0) | 250 | no | |
| 1 | $c_1$ | 250 | no | yes |
| 2 | $c_1c_2$ | 250 | no | yes |
| 3 | order 3 | 120 | yes | yes (counter only) |
| 4 | order 4 | 60 | yes | yes (counter only) |
| 5 | order 5 | 60 | yes | |
| 6 | order 6 | 60 | yes | yes (counter only) |
| 7 | order 8 (all of $c_8$) | 60 | yes | |
| 8 | sparse $(c_1, c_3)$ | 120 | yes | |
| 9 | $s>0$: $(x_{t-s}, c_1)$; $s=0$: $(wh, c_1)$ (or $c_1$ alone, tagged, outside a word) | 120 | yes | yes (counter only) |
| 10 | $s>0$: $(x_{t-s}, x_{t-2s})$; $s=0$: $(c_2c_3)$ | 120 | yes | |
| 11 | $s>0$: $(x_{t-s}, x_{t-s-1}, c_1)$; $s=0$: $(pwh, wh)$ inside a word, else $(pwh, c_1)$ tagged | 120 | yes | |
| 12 | $s>0$: $(x_{t-s}, x_{t-2s}, x_{t-3s})$; $s=0$: word trigram $((ppwh\ll32\,|\,pwh)\cdot K_2+wh) \bmod 2^{64}$ | 120 | yes | |
| 13 | $s>0$: $(x_{t-s-1}, x_{t-s}, x_{t-s+1})$ (last term 0 unless $s\ge2$); $s=0$: (byte at the same column of the previous line, $c_1$), tagged | 120 | yes | |
| 14 | $s>0$: $(c_1, c_2, x_{t-s})$; $s=0$: $(c_2c_3c_4)$ | 120 | yes | |

(Out-of-range history reads as 0. The column byte is $x_{pls+(t-ls)}$ if $pls+(t-ls)<ls$, else 0. Word hash: `wh = wh*0x2F0B4A27 + byte + 1 mod 2^32` over runs of letters, digits and bytes
$\ge128$; at the first other byte, if `wh` is non-zero then `ppwh = pwh`, `pwh = wh`; then `wh = 0`. Tags: model 9 outside a word uses
$c_1\,|\,\mathrm{0x1000000000}$; model 11 outside a word uses $(pwh\ll8)\,|\,c_1\,|\,\mathrm{0x2000000000000}$; model 13 ($s=0$) uses $(above\ll8)\,|\,c_1\,|\,\mathrm{0x3000000}$.)

**Slots.** Each model owns a table of $2^{tb_i}$ 32-bit slots: probability `P` (bits 0-15, init 32768), a field `F` (bits 16-23, init 0) and a check byte (bits 24-31, init 0).
`F` is an adaptation count for models 0-2 and in the lite profile, and a bit-history state for models 3-14 in the full profile. $tb=\min(\mathrm{membits},\max(16,\mathrm{bitlen}(8n)+1))$, with
$tb_0=\min(tb,12)$, $tb_1=\min(tb,19)$ and $tb_i=tb$ otherwise. Only the models active in the profile allocate a table.

**Addressing (nibble-local).** For model $i$ with context value $v$: $h=\big((v+1+i\cdot 0x10001)\cdot K\big) \bmod 2^{64}$,
$K=\mathrm{0x9E3779B97F4A7C15}$. At the first bit of each nibble (bit 7 and bit 3) compute $x_h=h$ for the high nibble and
$x_h=\big((h+c_0\cdot K_2)\bmod 2^{64}\cdot K\big)\bmod2^{64}$ for the low nibble ($c_0$ is the partial byte with its
leading 1, so 16..31; $K_2=\mathrm{0xD6E8FEB86659FD93}$). Then `base = (x_h >> (64-tb_i)) & ~15` and
`chk = (x_h >> (64-tb_i-8)) & 255`. The slot for a bit is `(base + node) & (2^{tb_i}-1)` where `node` $\in[1,15]$ is the
position in the 4-level binary tree of the current nibble ($c_0$ itself for the high nibble; for the low nibble
$(c_0 \,\&\, (2^{3-bp}-1))\,|\,2^{3-bp}$). All 15 slots of a nibble share one 64-byte line. **Collision handling:** if a slot's check byte differs from `chk`,
the slot is reset to $P=32768, F=0$ with the new check before use (always replace).

**Update** after coding bit $b$. Direct slots (`F` = count $C$): `P += (((b<<16) - P) * recip[C]) >> 16`; `C` increments up to the model's limit. History slots:
$cn=\min(sn[F],\mathrm{limit}_i)$; `P += (((b<<16) - P) * recip[cn]) >> 16`; `F = nex[F][b]`. The check byte is preserved.

**State maps.** For every history model $i$ and bit position $bp$ there is a table `SMP[(i*8+bp)*256 + state]` (16-bit, init `p0[state]`) with a count `SMN` (init 0). The state-map probability of the
state *before* the update is the model's second input, `stretch[SMP>>4]`; after the bit, `SMP += ((tgt - SMP) * recip[SMN]) >> 16` with $tgt=65535$ for a 1, 0 for a 0, and `SMN` increments up to 255.

**Model inputs.** `stretch[P>>4]` of every active model; one input per active match model; the state-map input of every history model; the bias 256. (Lane order is an implementation detail: the sums are exact integers.)

**Match models.** Two, each with a table of $2^{mb}$ positions, $mb=\min(22,\max(12,\mathrm{bitlen}(n)+1))$. Model M0:
minimum length 5, key $((c_8\,\&\,2^{40}-1)\cdot K \bmod 2^{64})\gg(64-mb)$, verification depth 32. Model M1: minimum length 12,
key: $g=0$; for $k=1..12$: $g=((g+x_{t-k}+1)\cdot K_2)\bmod2^{64}$; then $(g\cdot K \bmod 2^{64})\gg(64-mb)$; verification depth 48.
At each byte boundary: extend the current match if the predicted byte was right, else drop it; if none, look up the candidate and verify
backwards up to the depth, accepting length $\ge$ the minimum; then store the current position (once $t\ge$ minimum).
While the predicted byte agrees with the bits coded so far, a model contributes `±stretch[MP[len bucket]>>4]` (sign = predicted bit);
bucket $=\mathrm{len}$ for $<16$, else $\min(16+((\mathrm{len}-16)\gg2),32)$. Each model has its own `MP` (init 49152), updated
`+= ((hit<<16)-MP) >> 6`.

**Mixer network.** First layer: four weight sets (int16, $1.0=2^{13}$, saturating), each a dot product over the input vector: A selected by $(c_0, \text{M0 active})$ (512 sets), B by $c_1$ (256),
C by $c_2$ (256), D by $(\text{seen}\cdot2+\text{M0 active})$ (128 sets) where `seen` is a 6-bit mask, one bit per model 2..7 in order, set when that model's field `F` is non-zero (the context has been seen before).
Initial weights: 1228 for model and match inputs, 614 for history inputs, 0 for the bias. Each yields $d_k=\mathrm{clamp}_{\pm2047}((\sum w_ix_i)\gg13)$ and $p_k=\mathrm{squash}(d_k)$.
After the bit, set $k$ trains on its own error with a decaying rate: $lrq=8+\lfloor 60\cdot8192/(8192+t)\rfloor$ ($t$ = byte index; the lite profile uses a fixed 48),
$e_k=(((b\ll12)-p_k)\cdot lrq)\gg3$, $w_i \leftarrow \mathrm{sat}_{16}(w_i+((x_ie_k+32768)\gg16))$. Second layer: four weights (1.0 = 65536, initial 16384)
selected by $c_0$: $d=\mathrm{clamp}_{\pm2047}((\sum_k wf_kd_k)\gg16)$, $p_m=\mathrm{squash}(d)$; trained with
$e_f=(b\ll12)-p_m$, $wf_k \mathrel{+}= (d_ke_f)\gg14$.

**APM / SSE stages.** Two tables of $65536\times33$ 16-bit values, each row initialised to $\min(65535,16\cdot\mathrm{squash}(128(j-16)))$.
With $v=d+2048$, $a=v\gg7$, $f=v\,\&\,127$: $p_{a1}$ interpolates table 1 at row $(c_1\ll8\,|\,c_0)$ and $p_{a2}$ interpolates table 2 at row
$\big(((((c_8\,\&\,\mathrm{0xFFFF})\ll8)\,|\,c_0)\cdot K) \bmod 2^{64}\big)\gg48$, each as $(A[a](128-f)+A[a+1]f)\gg7$. Final 16-bit probability of a 1:
$p=\mathrm{clamp}_{[4,65532]}\big((32p_m+3p_{a1}+3p_{a2}+4)\gg3\big)$. After the bit, the two touched cells of each table move toward the outcome:
$A \mathrel{+}= (\mathrm{tgt}-A+\mathrm{rnd})\gg6$ with $(\mathrm{tgt},\mathrm{rnd})=(65535,63)$ for a 1 and $(0,0)$ for a 0.

**Lite profile** (`lite = 1`): active models 1, 2, 3, 4, 6, 9, all with plain counters (no bit histories, no state maps); match model M0 only; mixer sets A and B only with
$d=(d_A+d_B)\gg1$ (no second layer, no sets C and D), fixed learning rate 48; one APM (table 1) with $p=\mathrm{clamp}_{[4,65532]}((16p_m+3p_{a1}+2)\gg2)$.
Adding bit histories to lite gained only 1% for 20% more time, so lite stays on direct counters. It runs about 3x faster than the full profile (section 12).

## 4. What is and is not guaranteed

* **Guaranteed:** exact round-trip (checked by CRC-32 and by tests); no expansion beyond framing: a block is stored raw
  whenever coding it is not smaller.
* **Not guaranteed:** that AMDC-1 beats any other compressor. Results are measured, never assumed (section 12).
* The v1.0 bound $-\log_2 P_{\mathrm{mix}}(Y)\le -\log_2 P_j(Y)+\log_2 K$ holds only for the **Bayesian uniform mixture**. The
  logistic mixer has no such worst-case bound; its justification is empirical. The Bayesian formulation is retained as a
  possible future "AMDC-B" mode, not implemented.

## 5. Binary arithmetic coder (normative)

32-bit carry-less coder with **16-bit** probabilities ($p=P(\text{bit}=1)\in[4,65532]$). State $[x_1,x_2]$ starts at $[0,2^{32}-1]$.
$x_{mid}=x_1+\lfloor (x_2-x_1)\,p / 2^{16}\rfloor$, computed with a 64-bit product (**not** $((x_2-x_1)\gg16)\cdot p$: that truncation loses about 1% on real data);
bit 1 sets $x_2=x_{mid}$, bit 0 sets $x_1=x_{mid}+1$. While the top bytes of $x_1,x_2$
agree, shift one byte out ($x_1\ll8$, $x_2\ll8\,|\,255$). Flush writes $x_1$ as 4 big-endian bytes. The decoder preloads 4
bytes (missing bytes read as 0) and decodes bit $=[x\le x_{mid}]$.
Replaces v1.0 sections 5 and 6 (256-ary $M=2^{16}$ quantizer and range coder), which needed a per-symbol fractional-part sort and were far slower.
The probability clamp bounds the cost of any bit below at about 0.00009 bits, so highly redundant data has a floor of roughly 1 byte per 11,000 input bytes
(1 MiB of zeros encodes to 119 bytes, 1 MiB of 0xFF to 237). v1.1's 12-bit coder had a floor of about 1 byte per 2,800.

## 6. Block and transform selection

Blocks (default **16 MiB**) are independent: each starts with fresh model state, which allows parallel encode and decode.
Larger blocks share more context: on text, 1 MiB blocks cost about 4.6% against one 3 MiB block, and memory does not grow with the block
size (tables are capped by `membits`). Use `-b` to trade ratio for parallelism. For each block the encoder chooses among the 11 fixed candidates
$(\text{transform}, k, s)$:
`(id,-,0) (id,-,2) (id,-,4) (delta,1,1) (delta,2,2) (delta,4,4) (xor,1,1) (xor,4,4) (shuffle,2,0) (shuffle,4,0) (x86,-,0)`
plus up to two **detected strides** $(id,-,s)$ for blocks larger than the 12 KiB sample, plus raw.
$J_i = 8\cdot|\text{record}_i|$ uses the real encoded record length (headers included); ties go to the earlier candidate.

*Stride detection (encoder heuristic, not part of the format).* On up to 36,864 bytes from the middle of the block, for lags $s=5..\min(4096,\mathrm{len}/8)$,
count equal bytes $eq_s$ and sum absolute byte differences $sad_s$ over $x_i$ vs $x_{i-s}$ ($i\ge s_{max}$). Candidate 1 is the first lag maximising
$eq_s$ if $eq_s>\lfloor 3\bar{eq}/2\rfloor+64$; candidate 2 is the first lag minimising $sad_s$ if it differs from the first maximiser. This finds record lengths
of tables and row widths of raw images.

Levels: **0** lite model; candidate = x86 filter if the block looks like machine code (E8/E9 followed four bytes on by 00/FF at $\ge0.2\%$ of the positions of
up to 64 KiB from the middle), otherwise identity; **1** (default) full model; score every candidate on a 12 KiB sample (begin, middle, end thirds), then fully
encode the winner (raw if the sample is incompressible); **2** full model, fully encode all candidates. Level 1 is a heuristic: it can
miss the best candidate. Encoder search policy does not affect decodability.

## 7. Container (normative)

```
"AMDC" | version u8 = 4 | orig_len | block_size | block records | CRC-32 of original (u32 little-endian)
record:  0 | raw_len | bytes                                   (raw)
         1 | raw_len | xf u8 | params | s | cfg u8 | payload_len | payload   (context-mixed)
params:  xf 1,2,3 -> k ; xf 4 -> hdr_len, w, h, channels, colour_transform ; xf 0, 5 -> none
cfg:     bit 0 = lite, bits 1..7 = membits - 16  (membits > 24 is rejected)
```
All integers are unsigned LEB128 varints (at most 9 bytes). Decoder requirements, all enforced and tested:
reject bad magic/version, truncation, trailing bytes, `orig_len > max_output` (default 4 GiB, configurable), block size outside
$[1,2^{28}]$, `raw_len` outside $[1,\text{block\_size}]$ or overrunning `orig_len`, stride $>2^{24}$, a model configuration with `membits` $>24$, unknown transform ids ($>5$), invalid transform parameters,
and any CM block with `raw_len > 16384 × (payload_len + 8)` (the measured maximum is about 8800:1, for all-zero data; the theoretical floor above is about 11,000:1). All records are validated
**before** any output memory is allocated or any block decoded. A CRC mismatch is an error. Note: the block-size field is an
upper bound and the coder's flush tail has don't-care bytes, so a few single-byte flips still decode to the *correct*
output; the guarantee is "never silently wrong", which the tests assert.

## 8. Lossless image path (PGM/PPM, 8-bit)

Applies to binary `P5`/`P6` files with maxval 255. The whole file is one block (`xf 4`); header bytes and any trailing
bytes pass through verbatim, so reconstruction is **byte-exact**, not merely pixel-exact.

* Colour transform (`ct`): 0 none, or 1 "subtract green": $(R-G)\bmod256,\ G,\ (B-G)\bmod256$ (RGB only). The encoder tries both at level ≥ 1.
* Prediction per plane, raster order: MED $\hat I$ as in v1.0 (first row: left neighbour; first column: neighbour above; pixel (0,0): 0).
* Residual $e=(I-\hat I)\bmod256$, folded to a byte: $Z=2e$ for $e<128$, else $511-2e$ (a bijection on 0..255).
* Layout: header, plane 0 residuals, plane 1, plane 2, tail; coded with context stride $s=\text{width}$, so models 9, 10 and 11 see the residual above, two rows above, and above-left.

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

### Changes from v1.1 (format version 1 → 2)

1. Two additional context models (order 8, sparse $c_1c_3$): −1.0% size.
2. Second mixer weight set selected by $c_1$, averaged in the stretch domain: −1.7%.
3. APM/SSE final stage keyed by $(c_1,c_0)$: −1.6%.
4. Coder: 16-bit probabilities and exact 64-bit interval split: −0.07% on the corpus and an 8× lower redundancy floor.
5. Decoder expansion guard raised from 4096:1 to 16384:1 to admit the new floor. Version byte is now 2; version 1 archives are rejected.
6. Stage 1–3 gains were measured on the tuning corpus; section 12 reports a separate held-out corpus.

### Changes from v1.3 (format version 3 → 4)

Measured on 1.5 MiB slices of the 12 Silesia files with the raw model unless stated (v1.3 model: 5,428,676 bytes in total). Text subset = `dickens`, `reymont`, `webster`, `xml`, `nci`.

1. **Bit histories** on 12 of the 15 models (section 3): 229-state machine per slot, adaptive state maps keyed by (model, bit position, state), added as extra mixer inputs. −1.7% on the text subset.
2. **Mixer tuning:** lower learning rate with a decaying schedule, second-layer rate 1: about −0.9% on the text subset (each step was smaller than the one before; fixed rates 6 → 4 → 3 → 2 gave −0.3%, −0.4%, −0.6%).
3. **Three more contexts** (word trigram, column byte above in the previous line, skip context $c_2c_3c_4$; stride-mode analogues for binary data): −1.5% on the text subset, and almost all of it from the column context (`nci` −10.5%, `xml` −2.3%, natural text about −0.5%).
4. **Fourth mixer** selected by which context orders have been seen before: −0.3%. **Position-aware state maps:** −0.28%.
5. Net effect over all 12 data types: **−2.44%** (`mozilla` −7.3%, `ooffice` −7.1%, `nci` −13.3%, `reymont` −8.3%, `xml` −5.6%, `webster` −2.3%, `dickens` −1.1%, `osdb` −1.1%, and under 0.6% on `mr`, `samba`, `sao`, `x-ray`). Cost: about 40% lower throughput of the full profile.
6. On the dev set (first 3 MiB of each file) the real binary went from 27.285% to **26.747%**, level with zpaq -m5 (26.741%) there; the full-corpus comparison is in section 12.
7. The lite profile is unchanged (same sizes, same speed): bit histories there gained 1% for 20% more time, and the decaying learning rate made it worse (31.03% vs 30.71%), so it keeps plain counters and a fixed rate.
8. Rejected after measurement: state-map adaptation limits (no effect), other counter limits (±0.1%), count/probability state maps instead of real bit histories (−0.24%).
9. Format version byte is 4; versions 1 to 3 are rejected.

### Changes from v1.2 (format version 2 → 3)

Gains below are measured on 1.5 MiB slices of the 12 Silesia files with the raw model (total 18.45 MB; v1.2 model = 5,529,291 bytes).

1. Hash-verified slots (8-bit check, always-replace): −0.95%. Nibble-local addressing: about 0.3% larger, but one cache line per nibble per model.
2. Two more context models (order 5; word-bigram / above-left) and a second, long-range match model (min length 12): −0.28% from the match model.
3. Three first-layer mixers (adds $c_2$-selected set) plus a learned second layer: −0.67%. Mixer arithmetic is int16 with 16 padded lanes.
4. Second APM keyed by a hash of $(c_2c_1c_0)$: −0.12%.
5. Total for the model changes: −1.75% on the raw model.
6. **Default block 1 MiB → 16 MiB** (−4.6% on text). **`membits`** stored per block so memory is a decoder-visible, bounded setting.
7. **Lite profile** (level 0): six models, one match model, two mixers, one APM; about 2.3x faster.
8. **x86 filter** (transform 5) and **record-length detection**; level 0 enables the filter by a density heuristic.
9. Rejected after measurement: count/probability state-map inputs (−0.24% for +40% time), other adaptation limits (±0.1%), 64-bit scalar mixers (no speed difference vs int16).
10. Format version byte is 3; version 1 and 2 archives are rejected.

## 12. Benchmark protocol and results

**Protocol.** The complete Silesia corpus (12 real files, 211,938,580 bytes), downloaded from a public GitHub mirror. Every compressor runs
single-threaded on one core of the same machine (a 2.1 GHz virtualised Xeon, which is slow: bzip2 -9 compresses at only 10 MB/s here, so compare
the *ratios between rows*, not the absolute speeds, with other hardware). Every result is round-trip verified by SHA-256 (all passed).
Total archive size includes all headers. Competitors at strong standard settings: gzip -9, bzip2 -9, xz -9e, 7z LZMA2 -mx9, zstd --ultra -22 --long=27,
brotli -q11 -w24, lz4 -12, zpaq -method 3 and -method 5. AMDC-1 runs at its default 16 MiB blocks and `-m 22`; AMDC-B at its default 8 MiB blocks. Reproduce with
`python3 bench/bench_real.py --files DIR`; the raw per-file results are in `bench/results_silesia.csv`.

| tool | total bytes | ratio | compress MB/s | decompress MB/s |
|---|---|---|---|---|
| gzip -9 | 67,631,990 | 31.91% | 8.75 | 136 |
| bzip2 -9 | 54,506,769 | 25.72% | 10.64 | 26 |
| zstd -22 | 52,522,343 | 24.78% | 1.37 | 353 |
| brotli -11 | 49,564,563 | 23.39% | 0.40 | 218 |
| 7z LZMA2 | 48,757,480 | 23.01% | 1.71 | 61 |
| xz -9e | 48,456,004 | 22.86% | 1.33 | 62 |
| zpaq -m3 | 46,550,067 | 21.96% | 3.30 | 4 |
| **AMDC-B (BWT tier, experimental)** | 49,623,662 | 23.41% | 5.08 | 10 |
| **AMDC-1 level 0 (lite)** | 45,099,637 | 21.28% | 0.92 | 1 |
| **AMDC-1 level 1 (default)** | 40,285,344 | 19.01% | 0.26 | 0 |
| zpaq -m5 | 39,112,925 | 18.45% | 0.35 | 0 |
| *AMDC-F level 2 (fast format)* | 81,628,861 | 38.52% | 34.07 | 342 |
| *lz4 -12* | 77,372,720 | 36.51% | 8.56 | 684 |

**Reading the table honestly.**

* AMDC-1 level 1 is **16.9% smaller than xz -9e, 18.7% smaller than brotli -11 and 23.3% smaller than zstd -22**, and it is smaller than every mainstream tool
  (gzip, bzip2, xz, 7z, zstd, brotli, lz4) on **12 of 12 files**.
* **zpaq -m5 is still 3.0% smaller than AMDC-1 level 1** (it was 7.2% under format v3). AMDC-1 beats it on 3 files (`nci`, `sao`, `xml`); zpaq wins the other 9, mostly on text
  (`webster` 13.67% vs 15.20%, `dickens` 20.55% vs 21.55%). AMDC-1 is about 0.8x as fast as zpaq -m5 (slightly slower).
* **Lite is not a clear win over zpaq -m3**: it is 3.1% smaller but about 5x slower to decode (0.80 vs 4.0 MB/s).
* Context mixing is symmetric: decoding is as slow as encoding, about 219x (xz) to 1,257x (zstd) slower than their decoding. AMDC-1 is a ratio-first codec; AMDC-F is the fast tier.
* **AMDC-B** (BWT tier, `docs/AMDC-B_BWT_Tier.md`) is 9.0% smaller than bzip2 -9 and 2.4% *larger* than xz -9e, at 5.1 MB/s compress and 10.0 MB/s decompress per core
  (7x xz's compress speed). It is weak on executables (no x86 filter; `mozilla` 32.7% vs xz 26.1%). It does not reach 10-100 MB/s on one core; its blocks are independent so it should scale with cores, which was **not measured** (one-core machine).

| file | xz -9e | brotli-11 | zpaq -m5 | AMDC-B | AMDC-1 L0 | AMDC-1 L1 |
|---|---|---|---|---|---|---|
| dickens | 27.78 | 27.74 | 20.55 | 23.91 | 22.52 | 21.55 |
| mozilla | 26.11 | 27.08 | 23.51 | 32.73 | 28.56 | 23.99 |
| mr | 27.60 | 28.32 | 21.88 | 23.22 | 22.22 | 21.94 |
| nci | 4.32 | 4.53 | 3.73 | 4.17 | 4.60 | 3.67 |
| ooffice | 39.45 | 40.29 | 28.71 | 44.26 | 32.89 | 28.81 |
| osdb | 28.20 | 27.92 | 21.86 | 23.60 | 23.86 | 22.21 |
| reymont | 19.85 | 20.10 | 14.43 | 15.55 | 16.76 | 14.49 |
| samba | 17.31 | 17.43 | 14.13 | 19.03 | 15.98 | 14.28 |
| sao | 61.03 | 63.24 | 53.77 | 66.93 | 62.22 | 53.61 |
| webster | 20.19 | 20.33 | 13.67 | 17.70 | 16.18 | 15.20 |
| x-ray | 53.00 | 55.26 | 43.30 | 45.72 | 45.27 | 44.99 |
| xml | 8.14 | 8.05 | 6.12 | 7.48 | 6.91 | 5.99 |

**Memory and the "on the go" settings** (first 3 MiB of `dickens`, one core, peak resident memory including the input; `-m` = `membits`):

| level | `-m` | size | peak memory |
|---|---|---|---|
| 0 | 22 | 23.09% | 119 MB |
| 0 | 20 | 23.68% | 59 MB |
| 0 | 18 | 24.83% | 43 MB |
| 0 | 16 | 26.35% | 39 MB |
| 1 | 22 | 22.18% | 267 MB |
| 1 | 20 | 22.87% | 111 MB |
| 1 | 18 | 23.93% | 71 MB |
| 1 | 16 | 25.16% | 61 MB |
| AMDC-B | (8 MiB blocks) | 24.8% | 33 MB |

`-l 0 -m 20` (59 MB) and `-l 1 -m 20` (111 MB) are the sensible small-memory points. Memory depends on `-m`, not on the block size.

**Not measured / known limits.** Silesia is one corpus and mostly large files; small-file behaviour was only spot-checked (unit tests). The ablation table was not re-run for the 15-model design
(component gains are in section 11, measured on 1.5 MiB slices). Level 2 was not benchmarked on the full corpus. The 2.1 GHz virtualised core makes all absolute speeds pessimistic relative to a modern desktop,
and phone cores were not tested. Multi-core scaling of AMDC-1 and AMDC-B was not measured (one core available).
