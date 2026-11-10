#!/bin/sh
# C++ <-> Python reference interoperability. Level-0/small-input archives must be byte-identical.
set -e
cd "$(dirname "$0")/.."
BIN=build/amdc1
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
export PYTHONPATH=reference
python3 - "$T" <<'PY'
import os, random, struct, sys
t = sys.argv[1]; r = random.Random(3)
open(t + "/text.bin", "wb").write(open(os.__file__, "rb").read()[:20000])
open(t + "/f32.bin", "wb").write(b"".join(struct.pack("<f", 1000 + i * .37 + r.gauss(0, .5)) for i in range(3000)))
open(t + "/rand.bin", "wb").write(bytes(r.randrange(256) for _ in range(3000)))
open(t + "/zeros.bin", "wb").write(bytes(20000))
hdr = b"P6\n# c\n24 16\n255\n"
open(t + "/img.ppm", "wb").write(hdr + bytes((x*3 + y*2 + c*40 + r.randrange(4)) & 255 for y in range(16) for x in range(24) for c in range(3)) + b"tail")
open(t + "/empty.bin", "wb").write(b"")
PY
for f in text f32 rand zeros img empty; do
  in=$T/$f.bin; [ -f "$T/$f.ppm" ] && in=$T/$f.ppm
  for lvl in 0 1; do
    $BIN compress -l $lvl -j 1 "$in" "$T/c.amdc" 2>/dev/null
    python3 -m amdc1 compress -l $lvl "$in" "$T/p.amdc" 2>/dev/null
    cmp -s "$T/c.amdc" "$T/p.amdc" || { echo "MISMATCH archive $f level $lvl"; exit 1; }
    python3 -m amdc1 decompress "$T/c.amdc" "$T/p.out"; cmp "$in" "$T/p.out"
    $BIN decompress "$T/p.amdc" "$T/c.out"; cmp "$in" "$T/c.out"
  done
  echo "crosscheck ok: $f"
done
