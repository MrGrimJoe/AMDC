#!/usr/bin/env python3
"""Codec-only throughput (no disk I/O, no process start-up): every tool compresses and decompresses buffers already in memory,
best of 3, round trip verified. MB/s is always computed on the raw file bytes. usage: bench_inmem.py BUILD DATASETS OUT.json [dataset...]"""
import json, os, subprocess, sys, time
build, root, outjson = sys.argv[1:4]
names = sys.argv[4:] or sorted(os.listdir(root))
import lz4.frame as L, zstandard as Z

def best(f, reps=3):
    b = 1e9
    for _ in range(reps):
        t = time.perf_counter(); r = f(); b = min(b, time.perf_counter() - t)
    return b, r

out = {}
for ds in names:
    d = os.path.join(root, ds)
    tarb = subprocess.run(f"tar --sort=name --owner=0 --group=0 --numeric-owner --mtime=@0 -cf - -C {d} .", shell=True, capture_output=True, check=True).stdout
    rows = []
    def tool(cmd):
        raw, size, c, dd = subprocess.run(cmd, shell=True, capture_output=True, check=True, text=True).stdout.split()
        return int(raw), int(size), float(c), float(dd)
    raw = None
    for lv in (1, 2):
        raw, size, c, dd = tool(f"{build}/indep_tool bench -l {lv} {d}")
        rows.append(("AMDC-F independent files L%d" % lv, size, c, dd))
    for label, args in (("AMDC-S concat (g=1024,u=0,ext order)", "-g 1024 -u 0 -o ext"), ("AMDC-S interleave (g=32,u=1,extsize order)", "-g 32 -u 1 -o extsize")):
        for lv in (1, 2):
            raw, size, c, dd = tool(f"{build}/amdcs bench {args} -l {lv} {d}")
            rows.append((f"{label} L{lv}", size, c, dd))
    mb = raw / 1e6
    for lv in (0, 9):
        tc, comp = best(lambda: L.compress(tarb, compression_level=lv), 3 if lv == 0 else 2)
        td, back = best(lambda: L.decompress(comp)); assert back == tarb
        rows.append((f"tar+lz4 level {lv}", len(comp), mb / tc, mb / td))
    for lv in (1, 3, 19):
        cctx = Z.ZstdCompressor(level=lv)
        tc, comp = best(lambda: cctx.compress(tarb), 1 if lv == 19 else 3)
        dctx = Z.ZstdDecompressor()
        td, back = best(lambda: dctx.decompress(comp)); assert back == tarb
        rows.append((f"tar+zstd level {lv}", len(comp), mb / tc, mb / td))
    out[ds] = dict(raw=raw, rows=[dict(config=a, bytes=b, comp_mbs=c, dec_mbs=e) for a, b, c, e in rows])
    for a, b, c, e in rows: print(f"{ds:16s} {a:46s} {b:>11,d} {100*b/raw:6.2f}%  comp {c:7.0f} MB/s  dec {e:7.0f} MB/s", flush=True)
    json.dump(out, open(outjson, "w"), indent=1)
