#!/usr/bin/env python3
"""Honest comparison: AMDC-F independent files vs AMDC-S solid layouts vs tar+lz4 vs tar+zstd.
Every configuration is round-tripped to disk and diffed (diff -rq) against the original directory.
Reports archive size (all metadata included), best-of-N wall time, CPU time (user+sys of the whole process tree) and
peak RSS of the largest process in the tree. usage: bench_solid.py BUILD_DIR DATASETS_DIR OUT.json [dataset ...]"""
import json, os, shutil, subprocess, sys, tempfile, time

build, root, outjson = sys.argv[1], sys.argv[2], sys.argv[3]
names = sys.argv[4:] or sorted(os.listdir(root))
here = os.path.dirname(os.path.abspath(__file__))
codec = os.path.join(here, "tar_codec.py")
work = tempfile.mkdtemp(prefix="amdcs_bench_")
TAR = "tar --sort=name --owner=0 --group=0 --numeric-owner --mtime=@0"


def measure(cmd):
    t = time.time()
    p = subprocess.Popen(cmd, shell=True, executable="/bin/bash", stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    _, st, ru = os.wait4(p.pid, 0)
    wall = time.time() - t
    if os.waitstatus_to_exitcode(st) != 0:
        raise RuntimeError(f"failed: {cmd}\n{p.stderr.read().decode()[:500]}")
    return wall, ru.ru_utime + ru.ru_stime, ru.ru_maxrss / 1024.0


def du_bytes(path):
    if os.path.isfile(path): return os.path.getsize(path)
    return sum(os.path.getsize(os.path.join(d, f)) for d, _, fs in os.walk(path) for f in fs)


def best(cmd_fn, reps):
    res = [measure(cmd_fn()) for _ in range(reps)]
    return min(r[0] for r in res), min(r[1] for r in res), max(r[2] for r in res)


def run_config(name, src, comp_cmd, decomp_cmd, artifact, reps):
    shutil.rmtree(artifact, ignore_errors=True)
    def c():
        if os.path.isdir(artifact): shutil.rmtree(artifact)
        elif os.path.exists(artifact): os.remove(artifact)
        return comp_cmd
    cw, cc, cm = best(c, reps)
    size = du_bytes(artifact)
    dest = os.path.join(work, "dest")
    def d():
        shutil.rmtree(dest, ignore_errors=True); os.makedirs(dest)
        return decomp_cmd(dest)
    dw, dc, dm = best(d, max(1, reps - 1))
    ok = subprocess.run(["diff", "-rq", src, dest], capture_output=True).returncode == 0
    shutil.rmtree(dest, ignore_errors=True)
    if not ok: raise RuntimeError(f"ROUND TRIP MISMATCH: {name}")
    return dict(config=name, bytes=size, comp_s=cw, comp_cpu_s=cc, comp_mb=cm, dec_s=dw, dec_cpu_s=dc, dec_mb=dm, verified=True)


results = {}
if os.path.exists(outjson): results = json.load(open(outjson))
SOLID = [  # (label, args)
    ("AMDC-S concat (g=1024,u=0,ext order)", "-g 1024 -u 0 -o ext"),
    ("AMDC-S interleave (g=32,u=1,extsize order)", "-g 32 -u 1 -o extsize"),
]
for ds in names:
    if ds in results and len(results[ds]) >= 10: continue
    src = os.path.join(root, ds)
    raw = du_bytes(src)
    rows = []
    def add(r): rows.append(r); print(f"{ds:16s} {r['config']:46s} {r['bytes']:>11,d} {100*r['bytes']/raw:6.2f}%  comp {raw/1e6/r['comp_s']:7.0f} MB/s  dec {raw/1e6/r['dec_s']:7.0f} MB/s", flush=True)
    art = os.path.join(work, "art")
    for lv in (1, 2):
        add(run_config(f"AMDC-F independent files L{lv}", src, f"{build}/indep_tool pack -l {lv} {src} {art}",
                       lambda dest: f"{build}/indep_tool unpack {art} {dest}", art, 3))
    for label, args in SOLID:
        for lv in (1, 2):
            add(run_config(f"{label} L{lv}", src, f"{build}/amdcs pack {args} -l {lv} {src} {art}",
                           lambda dest: f"{build}/amdcs unpack {art} {dest}", art, 3))
    for lv in (0, 9):
        add(run_config(f"tar+lz4 level {lv}", src, f"{TAR} -cf - -C {src} . | python3 {codec} lz4 {lv} c > {art}",
                       lambda dest: f"python3 {codec} lz4 {lv} d < {art} | tar -xf - -C {dest}", art, 3))
    for lv in (1, 3, 19):
        add(run_config(f"tar+zstd level {lv}", src, f"{TAR} -cf - -C {src} . | python3 {codec} zstd {lv} c > {art}",
                       lambda dest: f"python3 {codec} zstd {lv} d < {art} | tar -xf - -C {dest}", art, 1 if lv == 19 else 3))
    results[ds] = rows
    results[ds + "__raw"] = raw
    json.dump(results, open(outjson, "w"), indent=1)
shutil.rmtree(work, ignore_errors=True)
