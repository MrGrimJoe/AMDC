#!/usr/bin/env python3
"""Is the gain from interleaving or just from putting similar files next to each other? Compresses the SAME tar content in
two orders (name order vs the extsize order AMDC-S uses) with lz4/zstd, and prints AMDC-S concat/interleave sizes for the same order.
usage: ordering_check.py BUILD DATASET_DIR [...]"""
import os, subprocess, sys
import lz4.frame as L, zstandard as Z
build = sys.argv[1]
def ext(p):
    s, d = p.rfind('/'), p.rfind('.')
    return '' if d < 0 or (s >= 0 and d < s) else p[d:]
for d in sys.argv[2:]:
    files = []
    for dp, _, fs in os.walk(d):
        for f in fs:
            p = os.path.join(dp, f)
            if os.path.isfile(p) and not os.path.islink(p): files.append(os.path.relpath(p, d))
    raw = sum(os.path.getsize(os.path.join(d, f)) for f in files)
    def tar_of(order):
        lst = "\n".join(order).encode() + b"\n"
        return subprocess.run(["tar", "--owner=0", "--group=0", "--numeric-owner", "--mtime=@0", "--no-recursion", "-cf", "-", "-C", d, "-T", "-"], input=lst, capture_output=True, check=True).stdout
    byname = sorted(files)
    byext = sorted(files, key=lambda f: (ext(f), os.path.getsize(os.path.join(d, f)), f))
    print(os.path.basename(d.rstrip('/')), f"raw {raw/1e6:.1f} MB")
    for label, order in (("name order", byname), ("ext,size order", byext)):
        t = tar_of(order)
        print(f"  tar {label:15s} lz4-9 {100*len(L.compress(t, compression_level=9))/raw:5.1f}%   zstd-3 {100*len(Z.ZstdCompressor(level=3).compress(t))/raw:5.1f}%   zstd-19 {100*len(Z.ZstdCompressor(level=19).compress(t))/raw:5.1f}%")
    for label, args in (("AMDC-S concat   g=1024 u=0 extsize", "-g 1024 -u 0 -o extsize"), ("AMDC-S interleave g=32 u=1 extsize", "-g 32 -u 1 -o extsize")):
        for lv in (1, 2):
            out = subprocess.run(f"{build}/amdcs bench {args} -l {lv} {d}", shell=True, capture_output=True, text=True, check=True).stdout.split()
            print(f"  {label} L{lv}: {100*int(out[1])/raw:5.1f}%")
