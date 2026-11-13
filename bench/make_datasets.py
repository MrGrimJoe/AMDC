#!/usr/bin/env python3
"""Builds the benchmark directories from files already on this machine (regular files only, symlinks skipped, size-capped
so a run takes minutes). usage: make_datasets.py OUTDIR"""
import os, shutil, sys, glob

out = sys.argv[1]
CAP = 48 << 20

def copy_set(name, files, cap=CAP):
    d = os.path.join(out, name); shutil.rmtree(d, ignore_errors=True)
    total = n = 0
    for src, rel in files:
        try:
            if os.path.islink(src) or not os.path.isfile(src): continue
            sz = os.path.getsize(src)
            if total + sz > cap: continue
            dst = os.path.join(d, rel); os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(src, dst); total += sz; n += 1
        except OSError: pass
    print(f"{name:16s} {n:6d} files {total/1e6:8.1f} MB")

def walk(root, pred=lambda p: True, strip=None):
    for dp, dn, fn in sorted(os.walk(root)):
        dn.sort()
        for f in sorted(fn):
            p = os.path.join(dp, f)
            if pred(p): yield p, os.path.relpath(p, strip or root)

copy_set("source_py",     walk("/usr/lib/python3.12", lambda p: p.endswith(".py")))
copy_set("source_versions",
         [(s, "v311/" + r) for s, r in walk("/usr/lib/python3.11", lambda p: p.endswith(".py"))] +
         [(s, "v312/" + r) for s, r in walk("/usr/lib/python3.12", lambda p: p.endswith(".py"))] +
         [(s, "v313/" + r) for s, r in walk("/usr/lib/python3.13", lambda p: p.endswith(".py"))], cap=200 << 20)
copy_set("headers",       walk("/usr/include"))
copy_set("binaries",      walk("/usr/bin"))
copy_set("libraries",     walk("/usr/lib/x86_64-linux-gnu", lambda p: ".so" in os.path.basename(p)))
copy_set("text_docs",     walk("/usr/share/doc", lambda p: not p.endswith((".gz", ".png", ".jpg"))))
allf = [p for p, _ in walk("/usr/share")] + [p for p, _ in walk("/usr/lib") if "/python3" not in p]
copy_set("mixed",         [(p, os.path.relpath(p, "/usr")) for p in allf[::29]])
# negative control: unrelated incompressible files
d = os.path.join(out, "random_control"); shutil.rmtree(d, ignore_errors=True); os.makedirs(d)
for i in range(300): open(os.path.join(d, f"r{i:03d}.bin"), "wb").write(os.urandom(100 << 10))
print(f"{'random_control':16s}    300 files   30.7 MB")
