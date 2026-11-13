#!/usr/bin/env python3
"""AMDC-F C++ <-> pure-Python reference cross-check (run via `make crosscheck-fast`).
 1. C++ archives (levels 1/2, several block sizes, 1 and 3 threads, with/without checksum) decode identically in Python.
 2. Python archives decode identically in the C++ binary.
 3. Mutated archives: both implementations must agree on accept/reject and, when accepted, on the bytes."""
import os, random, subprocess, sys, tempfile
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "reference"))
import amdcf

BIN = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "amdcf")
T = tempfile.mkdtemp()
r = random.Random(7)


def cpp(*args):
    return subprocess.run([BIN, *args], capture_output=True).returncode


def cpp_compress(data, lvl, bs, j, chk):
    open(T + "/in", "wb").write(data)
    a = ["compress", "-l", str(lvl), "-b", str(bs), "-j", str(j)] + ([] if chk else ["--no-check"])
    assert cpp(*a, T + "/in", T + "/c") == 0
    return open(T + "/c", "rb").read()


def cpp_decompress(arc):
    open(T + "/a", "wb").write(arc)
    if cpp("decompress", "-j", "2", T + "/a", T + "/o") != 0:
        return None
    return open(T + "/o", "rb").read()


def py_decompress(arc):
    try:
        return amdcf.decompress(arc)
    except amdcf.FastError:
        return None


def pattern(n, period):
    p = bytes(r.randrange(256) for _ in range(period))
    return (p * (n // period + 1))[:n]


words = [b"the ", b"quick ", b"brown ", b"fox ", b"jumps ", b"over ", b"lazy ", b"dog. ", b"alpha ", b"beta "]
text = b"".join(r.choice(words) for _ in range(30000))[:150000]
inputs = {
    "empty": b"", "one": b"x", "text": text, "zeros": bytes(70000), "fill": b"\xAB" * 9000,
    "rand": bytes(r.randrange(256) for _ in range(20000)),
    "mixed": text[:40000] + bytes(30000) + bytes(r.randrange(256) for _ in range(9000)) + text[:20000],
}
for per in (2, 3, 7, 15, 16, 17, 31, 64, 1000):
    inputs["period%d" % per] = pattern(60000, per)

fails = 0
for name, data in inputs.items():
    for lvl in (1, 2):
        for bs, j, chk in ((4096, 1, True), (65536, 3, True), (262144, 1, False)):
            arc = cpp_compress(data, lvl, bs, j, chk)
            if py_decompress(arc) != data:
                print("FAIL cpp->python", name, lvl, bs, j, chk); fails += 1
            pyarc = amdcf.compress(data, bs, chk)
            if cpp_decompress(pyarc) != data:
                print("FAIL python->cpp", name, bs, chk); fails += 1
            if cpp_decompress(arc) != data:
                print("FAIL cpp->cpp", name); fails += 1
    # pages
    for pg in (data[:4096], data[4096:8192]):
        if len(pg) == 4096:
            enc = amdcf.page_compress(pg)
            if enc is not None and amdcf.page_decompress(enc, 4096) != pg:
                print("FAIL page roundtrip", name); fails += 1
    print("crosscheck-fast ok: %s" % name)

# mutation differential: both decoders must agree
agree = rejected = 0
for name in ("text", "mixed", "period7", "period16"):
    arc = cpp_compress(inputs[name], 1, 65536, 1, False)   # no checksum: exercises the block decoders themselves
    for it in range(250):
        m = bytearray(arc)
        for _ in range(1 + r.randrange(3)):
            m[r.randrange(len(m))] = r.randrange(256)
        if r.randrange(6) == 0:
            m = m[:r.randrange(len(m))]
        a, b = py_decompress(bytes(m)), cpp_decompress(bytes(m))
        if a != b:
            print("FAIL mutation disagreement", name, it); fails += 1
        agree += 1
        rejected += a is None
print("mutation differential: %d cases, %d rejected by both, 0 disagreements" % (agree, rejected) if not fails else "FAILED")
sys.exit(1 if fails else 0)
