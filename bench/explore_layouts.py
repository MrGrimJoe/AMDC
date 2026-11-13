#!/usr/bin/env python3
"""Size/time sweep of AMDC-S layouts (group size, interleave unit, file order) per dataset -> TSV on stdout.
usage: explore_layouts.py AMDCS DATASETS_DIR [dataset ...]"""
import itertools, os, subprocess, sys, tempfile, time
amdcs, root = sys.argv[1], sys.argv[2]
names = sys.argv[3:] or sorted(os.listdir(root))
print("dataset\torder\tgroup\tunit\tbytes\tseconds", flush=True)
tmp = tempfile.mkdtemp()
for ds in names:
    d = os.path.join(root, ds)
    cfgs = [("name", 1, 1)] + [(o, g, u) for o in ("name", "ext", "extsize") for g in (2, 4, 8, 16) for u in (0, 1, 2, 4)]
    for o, g, u in cfgs:
        out = os.path.join(tmp, "a.amdss")
        t = time.time()
        subprocess.run([amdcs, "pack", "-g", str(g), "-u", str(u), "-o", o, d, out], check=True, capture_output=True)
        print(f"{ds}\t{o}\t{g}\t{u}\t{os.path.getsize(out)}\t{time.time()-t:.2f}", flush=True)
