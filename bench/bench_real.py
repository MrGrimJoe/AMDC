#!/usr/bin/env python3
"""Benchmark amdc1/amdcf against common compressors on real files.

Every result is round-trip verified (sha256). Single thread everywhere so the
numbers are comparable. Output: CSV lines on stdout + summary table at the end.

  python3 bench/bench_real.py --files DIR_OR_FILES... [--tools a,b,c] [--csv out.csv]
"""
import argparse, hashlib, os, shutil, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
B = os.path.join(ROOT, "build")

# name -> (compress argv template, decompress argv template); {i} input, {o} output
TOOLS = {
    "gzip-9":     (["gzip", "-9", "-c", "{i}"], ["gzip", "-dc", "{i}"], "stdout"),
    "bzip2-9":    (["bzip2", "-9", "-c", "{i}"], ["bzip2", "-dc", "{i}"], "stdout"),
    "xz-9e":      (["xz", "-9e", "-T1", "-c", "{i}"], ["xz", "-dc", "{i}"], "stdout"),
    "zstd-19":    (["zstd", "-19", "-T1", "-q", "-c", "{i}"], ["zstd", "-dc", "{i}"], "stdout"),
    "zstd-22":    (["zstd", "--ultra", "-22", "--long=27", "-T1", "-q", "-c", "{i}"],
                   ["zstd", "-dc", "--long=27", "{i}"], "stdout"),
    "brotli-11":  (["brotli", "-q", "11", "-w", "24", "-c", "{i}"], ["brotli", "-dc", "{i}"], "stdout"),
    "7z-lzma2":   (["7z", "a", "-txz", "-mx=9", "-mmt=1", "-so", "x.xz", "{i}"], None, "7z"),
    "lz4-12":     (["lz4", "-12", "-q", "-c", "{i}"], ["lz4", "-dc", "{i}"], "stdout"),
    "zpaq-m5":    (None, None, "zpaq5"),
    "zpaq-m3":    (None, None, "zpaq3"),
    "amdcf-2":    ([os.path.join(B, "amdcf"), "compress", "-l", "2", "-j", "1", "{i}", "{o}"],
                   [os.path.join(B, "amdcf"), "decompress", "-j", "1", "{i}", "{o}"], "file"),
    "amdc1-0":    ([os.path.join(B, "amdc1"), "compress", "-l", "0", "-j", "1", "{i}", "{o}"],
                   [os.path.join(B, "amdc1"), "decompress", "-j", "1", "{i}", "{o}"], "file"),
    "amdc1-1":    ([os.path.join(B, "amdc1"), "compress", "-l", "1", "-j", "1", "{i}", "{o}"],
                   [os.path.join(B, "amdc1"), "decompress", "-j", "1", "{i}", "{o}"], "file"),
}


def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""):
            h.update(c)
    return h.hexdigest()


def run(argv, out=None):
    t = time.perf_counter()
    if out:
        with open(out, "wb") as f:
            r = subprocess.run(argv, stdout=f, stderr=subprocess.PIPE)
    else:
        r = subprocess.run(argv, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    dt = time.perf_counter() - t
    if r.returncode != 0:
        raise RuntimeError("%s failed: %s" % (argv[0], r.stderr.decode(errors="replace")[:300]))
    return dt


def sub(tpl, i, o):
    return [a.replace("{i}", i).replace("{o}", o) for a in tpl]


def bench_one(tool, path, tmp):
    comp, decomp, mode = TOOLS[tool]
    arc = os.path.join(tmp, "arc")
    dec = os.path.join(tmp, "dec")
    for p in (arc, dec):
        if os.path.exists(p):
            if os.path.isdir(p): shutil.rmtree(p)
            else: os.remove(p)
    if mode == "stdout":
        ct = run(sub(comp, path, arc), out=arc)
        dt = run(sub(decomp, arc, dec), out=dec)
        size = os.path.getsize(arc)
    elif mode == "file":
        ct = run(sub(comp, path, arc))
        dt = run(sub(decomp, arc, dec))
        size = os.path.getsize(arc)
    elif mode == "7z":
        arc = arc + ".7z"
        ct = run(["7z", "a", "-t7z", "-m0=lzma2", "-mx=9", "-mmt=1", "-bd", "-bso0", arc, path])
        dd = os.path.join(tmp, "dec7")
        os.makedirs(dd, exist_ok=True)
        dt = run(["7z", "x", "-bd", "-bso0", "-y", "-o" + dd, arc])
        dec = os.path.join(dd, os.path.basename(path))
        size = os.path.getsize(arc)
    elif mode.startswith("zpaq"):
        m = mode[-1]
        arc = arc + ".zpaq"
        ct = run(["zpaq", "a", arc, path, "-method", m, "-threads", "1", "-noattributes"], None)
        dd = os.path.join(tmp, "decz")
        os.makedirs(dd, exist_ok=True)
        dt = run(["zpaq", "x", arc, "-to", dd, "-threads", "1", "-force"])
        # zpaq stores the full path
        found = None
        for r, _, fs in os.walk(dd):
            for f in fs:
                if f == os.path.basename(path): found = os.path.join(r, f)
        dec = found
        size = os.path.getsize(arc)
    ok = dec is not None and os.path.exists(dec) and sha(dec) == sha(path)
    for p in (arc,):
        if os.path.exists(p): os.remove(p)
    for d in ("dec7", "decz"):
        shutil.rmtree(os.path.join(tmp, d), ignore_errors=True)
    if os.path.exists(os.path.join(tmp, "dec")): os.remove(os.path.join(tmp, "dec"))
    return size, ct, dt, ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--files", nargs="+", required=True)
    ap.add_argument("--tools", default=",".join(TOOLS))
    ap.add_argument("--csv")
    a = ap.parse_args()
    files = []
    for f in a.files:
        if os.path.isdir(f):
            files += sorted(os.path.join(f, x) for x in os.listdir(f))
        else:
            files.append(f)
    tools = a.tools.split(",")
    tmp = tempfile.mkdtemp(prefix="bench")
    rows = []
    out = open(a.csv, "w") if a.csv else None
    hdr = "tool,file,orig,comp,ratio_pct,comp_s,decomp_s,ok"
    print(hdr, flush=True)
    if out: out.write(hdr + "\n")
    for p in files:
        orig = os.path.getsize(p)
        for t in tools:
            try:
                size, ct, dt, ok = bench_one(t, p, tmp)
            except Exception as e:
                print("# %s %s: %s" % (t, os.path.basename(p), e), file=sys.stderr, flush=True)
                continue
            rows.append((t, os.path.basename(p), orig, size, ct, dt, ok))
            line = "%s,%s,%d,%d,%.3f,%.2f,%.2f,%s" % (t, os.path.basename(p), orig, size, 100.0 * size / orig, ct, dt, "OK" if ok else "FAIL")
            print(line, flush=True)
            if out: out.write(line + "\n"); out.flush()
    shutil.rmtree(tmp, ignore_errors=True)
    # summary
    print("\n%-10s %12s %8s %9s %9s %6s" % ("tool", "total_comp", "ratio%", "MB/s_c", "MB/s_d", "fails"))
    for t in tools:
        rs = [r for r in rows if r[0] == t]
        if not rs: continue
        o = sum(r[2] for r in rs); c = sum(r[3] for r in rs)
        ct = sum(r[4] for r in rs); dt = sum(r[5] for r in rs)
        print("%-10s %12d %8.3f %9.2f %9.2f %6d" % (t, c, 100.0 * c / o, o / ct / 1e6, o / dt / 1e6, sum(1 for r in rs if not r[6])))


if __name__ == "__main__":
    main()
