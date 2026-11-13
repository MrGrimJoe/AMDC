#!/usr/bin/env python3
"""Turns inmem.json + results.json (from bench_inmem.py / bench_solid.py) into the markdown tables used in docs/AMDC-S_Solid_Mode.md.
usage: report.py inmem.json results.json"""
import json, sys
im, fl = json.load(open(sys.argv[1])), json.load(open(sys.argv[2]))
order = ["binaries", "libraries", "headers", "source_py", "source_versions", "text_docs", "mixed", "random_control"]
cfgs = [r["config"] for r in im[order[0]]["rows"]]
short = {c: c.replace("AMDC-F independent files", "AMDC-F indep").replace("AMDC-S concat (g=1024,u=0,ext order)", "AMDC-S concat").replace("AMDC-S interleave (g=32,u=1,extsize order)", "AMDC-S interleave").replace("tar+", "tar+") for c in cfgs}
def table(title, cell, unit):
    print(f"\n**{title}**\n")
    print("| dataset (raw MB) | " + " | ".join(short[c] for c in cfgs) + " |")
    print("|---|" + "---:|" * len(cfgs))
    for ds in order:
        rows = {r["config"]: r for r in im[ds]["rows"]}
        print(f"| {ds} ({im[ds]['raw']/1e6:.1f}) | " + " | ".join(cell(ds, rows[c], c) for c in cfgs) + " |")
table("Archive size as % of raw bytes (metadata included; independent = sum of per-file outputs)", lambda ds, r, c: f"{100*r['bytes']/im[ds]['raw']:.1f}", "%")
table("Codec-only compression throughput, MB/s of raw data (in memory, no disk I/O)", lambda ds, r, c: f"{r['comp_mbs']:.0f}", "")
table("Codec-only decompression throughput, MB/s of raw data (in memory, no disk I/O)", lambda ds, r, c: f"{r['dec_mbs']:.0f}", "")
def fcell(key, fmt):
    def f(ds, r, c):
        x = [y for y in fl[ds] if y["config"] == c][0]
        return fmt(x, fl[ds + "__raw"])
    return f
table("End-to-end pack from directory: wall seconds (best of 3, includes reading files and writing the archive)", fcell("comp_s", lambda x, raw: f"{x['comp_s']:.2f}"), "")
table("End-to-end unpack to directory: wall seconds (best of 2, includes writing every file)", fcell("dec_s", lambda x, raw: f"{x['dec_s']:.2f}"), "")
table("Pack CPU seconds (user+sys, whole process tree)", fcell("comp_cpu_s", lambda x, raw: f"{x['comp_cpu_s']:.2f}"), "")
table("Pack peak memory, MB (largest process in the tree)", fcell("comp_mb", lambda x, raw: f"{x['comp_mb']:.0f}"), "")
table("Unpack peak memory, MB (largest process in the tree)", fcell("dec_mb", lambda x, raw: f"{x['dec_mb']:.0f}"), "")
