#!/usr/bin/env python3
"""Benchmark amdc1 against available external compressors (spec section 12).

usage: bench.py [DIR ...]      # no DIR: use a deterministic synthetic + system-file corpus
Reports total archive size (headers included), wall time, and byte-exact round-trip status.
Competitors run at their strongest standard settings; AMDC is shown at level 1 and level 2.
"""
import json, os, random, shutil, struct, subprocess, sys, tempfile, time, glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "amdc1")
CODECS = [
    ("gzip-9", ["gzip", "-9", "-c"], None),
    ("bzip2-9", ["bzip2", "-9", "-c"], None),
    ("xz-9e", ["xz", "-9e", "-c"], None),
    ("zstd-19", ["zstd", "-19", "--ultra", "-c", "-q"], None),
    ("brotli-q11", ["brotli", "-q", "11", "-c"], None),
]


def synth(d):
    r = random.Random(11)
    out = {}
    out["json"] = ("[" + ",".join('{"id":%d,"name":"user%d","score":%.2f,"tags":["a","b"]}' % (
        i, r.randint(0, 999), r.random() * 100) for i in range(4000)) + "]").encode()
    out["csv"] = "\n".join("%d,%.3f,%d,%s" % (i, r.gauss(50, 9), r.randint(0, 99), r.choice(["red", "green", "blue"]))
                          for i in range(8000)).encode()
    out["float32"] = b"".join(struct.pack("<f", 1000 + i * .37 + r.gauss(0, .5)) for i in range(60000))
    out["int32_ramp"] = b"".join(struct.pack("<I", 1000 + i * 7 + r.randrange(3)) for i in range(60000))
    w, h = 160, 120
    out["image.ppm"] = b"P6\n%d %d\n255\n" % (w, h) + bytes(
        min(255, max(0, int(128 + 90 * ((x / w) - (y / h)) + 30 * ((x // 16 + y // 16) % 2) + r.gauss(0, 3) + c * 9)))
        for y in range(h) for x in range(w) for c in range(3))
    out["random"] = bytes(r.randrange(256) for _ in range(100000))
    out["zeros"] = bytes(500000)
    for name, path in (("python-src", "/usr/lib/python3.12/argparse.py"), ("python-src2", "/usr/lib/python3.12/typing.py"),
                       ("elf-binary", shutil.which("gzip") or "/bin/ls")):
        if os.path.exists(path):
            out[name] = open(path, "rb").read()[:400000]
    for n, b in out.items():
        open(os.path.join(d, n), "wb").write(b)
    return sorted(out)


def run(cmd, data=None, path=None):
    t = time.time()
    p = subprocess.run(cmd + ([path] if path else []), input=data, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    return p.stdout, time.time() - t


def main():
    if not os.path.exists(BIN):
        sys.exit("build first: make")
    tmp = tempfile.mkdtemp()
    if len(sys.argv) > 1:
        files = [f for d in sys.argv[1:] for f in sorted(glob.glob(os.path.join(d, "**", "*"), recursive=True)) if os.path.isfile(f)]
    else:
        files = [os.path.join(tmp, n) for n in synth(tmp)]
    avail = [(n, c) for n, c, _ in CODECS if shutil.which(c[0])]
    cols = [n for n, _ in avail] + ["amdc1-L1", "amdc1-L2"]
    print("%-16s %9s " % ("file", "bytes") + " ".join("%10s" % c for c in cols))
    tot = {c: 0 for c in cols}; tt = {c: 0.0 for c in cols}; total = 0; allok = True
    for f in files:
        raw = open(f, "rb").read()
        row = []
        for n, c in avail:
            o, dt = run(c, raw); tot[n] += len(o); tt[n] += dt; row.append(len(o))
        for lvl in (1, 2):
            a = os.path.join(tmp, "x.amdc"); b = os.path.join(tmp, "x.out")
            t = time.time()
            subprocess.run([BIN, "compress", "-l", str(lvl), f, a], stderr=subprocess.DEVNULL, check=True)
            dt = time.time() - t
            subprocess.run([BIN, "decompress", a, b], check=True)
            ok = open(b, "rb").read() == raw; allok &= ok
            sz = os.path.getsize(a); n = "amdc1-L%d" % lvl
            tot[n] += sz; tt[n] += dt; row.append(sz if ok else -1)
        total += len(raw)
        print("%-16s %9d " % (os.path.basename(f)[:16], len(raw)) + " ".join("%10d" % v for v in row))
    print("%-16s %9d " % ("TOTAL", total) + " ".join("%10d" % tot[c] for c in cols))
    print("%-16s %9s " % ("ratio", "") + " ".join("%9.2f%%" % (100.0 * tot[c] / total) for c in cols))
    print("%-16s %9s " % ("time (s)", "") + " ".join("%10.2f" % tt[c] for c in cols))
    print("round-trip: %s" % ("ALL EXACT" if allok else "FAILURES"))
    shutil.rmtree(tmp)


main()
