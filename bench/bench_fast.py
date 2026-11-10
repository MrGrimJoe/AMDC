#!/usr/bin/env python3
"""Decode-speed comparison: amdcf vs lz4 / zstd / zlib / xz on one file, same machine, single thread.
usage: bench_fast.py FILE [REPS]      (needs build/amdcf; `pip install lz4 zstandard` for the extra rows)
Python-module timings call the C libraries on a whole buffer, so interpreter overhead is negligible."""
import lzma, os, re, subprocess, sys, time, zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
path = sys.argv[1]
reps = int(sys.argv[2]) if len(sys.argv) > 2 else 20
raw = open(path, "rb").read()
mb = len(raw) / 1e6
rows = []


def timeit(fn):
    best = 1e9
    for _ in range(reps):
        t = time.perf_counter(); fn(); best = min(best, time.perf_counter() - t)
    return best


def add(name, comp, decomp):
    c = comp(raw)
    assert decomp(c) == raw, name
    rows.append((name, len(c), mb / timeit(lambda: decomp(c))))


for lvl, tag in ((1, "amdcf-1"), (2, "amdcf-2")):
    out = subprocess.run([os.path.join(ROOT, "build", "amdcf"), "bench", "-l", str(lvl), "-r", str(reps), path],
                         capture_output=True, text=True, check=True).stdout
    size = int(re.search(r"-> (\d+) B \(", out).group(1))
    spd = float(re.search(r"single block, 1 thread, no checksum\):\s+(\d+) MB/s", out).group(1))
    cspd = float(re.search(r"container, -j 1, into preallocated buffer, checksum verified\):\s+(\d+) MB/s", out).group(1))
    rows.append((tag + " (raw block)", size, spd)); rows.append((tag + " (container+checksum)", size, cspd))
try:
    import lz4.block as L
    add("lz4 default", lambda b: L.compress(b, store_size=False), lambda c: L.decompress(c, uncompressed_size=len(raw)))
    add("lz4 hc-9", lambda b: L.compress(b, mode="high_compression", compression=9, store_size=False),
        lambda c: L.decompress(c, uncompressed_size=len(raw)))
except ImportError:
    pass
try:
    import zstandard as Z
    for lv in (1, 19):
        d = Z.ZstdDecompressor()
        add("zstd-%d" % lv, lambda b, lv=lv: Z.ZstdCompressor(level=lv).compress(b), lambda c: d.decompress(c, max_output_size=len(raw)))
except ImportError:
    pass
add("zlib-6", lambda b: zlib.compress(b, 6), zlib.decompress)
add("xz-6", lambda b: lzma.compress(b, preset=6), lzma.decompress)
print("%-28s %10s %8s %12s" % (os.path.basename(path), "bytes", "ratio", "decode MB/s"))
for n, s, v in rows:
    print("%-28s %10d %7.2f%% %12.0f" % (n, s, 100.0 * s / len(raw), v))
