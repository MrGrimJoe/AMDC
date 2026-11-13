#!/usr/bin/env python3
"""Second sweep: larger groups, plain solid (unit 0), stripe sizes, and level 2. TSV on stdout.
usage: explore_layouts2.py AMDCS DATASETS_DIR [dataset ...]"""
import os, subprocess, sys, tempfile, time
amdcs, root = sys.argv[1], sys.argv[2]
names = sys.argv[3:] or sorted(os.listdir(root))
print("dataset\torder\tgroup\tunit\tstripeKiB\tlevel\tbytes\tseconds", flush=True)
tmp = tempfile.mkdtemp()
cfgs = []
for o in ("name", "ext", "extsize"):
    for g in (16, 64, 1024):
        cfgs.append((o, g, 0, 256, 1))
for g in (16, 32, 64):
    for u in (1, 2):
        cfgs.append(("extsize", g, u, 256, 1))
for sb in (64, 1024):
    cfgs += [("extsize", 16, 0, sb, 1), ("extsize", 16, 1, sb, 1), ("name", 1024, 0, sb, 1)]
cfgs += [("extsize", 16, 0, 256, 2), ("extsize", 16, 1, 256, 2), ("name", 1024, 0, 256, 2), ("name", 1, 1, 256, 2)]
for ds in names:
    for o, g, u, sb, lv in cfgs:
        out = os.path.join(tmp, "a.amds")
        t = time.time()
        subprocess.run([amdcs, "pack", "-g", str(g), "-u", str(u), "-o", o, "-s", str(sb), "-l", str(lv), os.path.join(root, ds), out], check=True, capture_output=True)
        print(f"{ds}\t{o}\t{g}\t{u}\t{sb}\t{lv}\t{os.path.getsize(out)}\t{time.time()-t:.2f}", flush=True)
