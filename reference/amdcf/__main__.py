import sys
from . import FastError, compress, decompress

usage = "usage: python3 -m amdcf compress [-b BYTES] [--no-check] IN OUT | decompress IN OUT"
a = sys.argv[1:]
try:
    if len(a) >= 3 and a[0] == "compress":
        bs, chk, rest = 256 << 10, True, a[1:]
        while rest and rest[0].startswith("-"):
            if rest[0] == "-b": bs = int(rest[1]); rest = rest[2:]
            elif rest[0] == "--no-check": chk = False; rest = rest[1:]
            else: raise SystemExit(usage)
        open(rest[1], "wb").write(compress(open(rest[0], "rb").read(), bs, chk))
    elif len(a) == 3 and a[0] == "decompress":
        open(a[2], "wb").write(decompress(open(a[1], "rb").read()))
    else:
        raise SystemExit(usage)
except FastError as e:
    sys.exit("amdcf: " + str(e))
