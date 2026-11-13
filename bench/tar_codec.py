#!/usr/bin/env python3
"""stdin -> stdout codec used for the tar+lz4 / tar+zstd baselines (python-lz4 frame format, python-zstandard).
usage: tar_codec.py lz4|zstd LEVEL c|d"""
import sys
codec, level, mode = sys.argv[1], int(sys.argv[2]), sys.argv[3]
inp, out = sys.stdin.buffer, sys.stdout.buffer
if codec == "lz4":
    import lz4.frame as L
    data = inp.read()
    out.write(L.compress(data, compression_level=level) if mode == "c" else L.decompress(data))
else:
    import zstandard as Z
    if mode == "c":
        out.write(Z.ZstdCompressor(level=level).compress(inp.read()))
    else:
        with Z.ZstdDecompressor().stream_reader(inp) as r:
            while True:
                b = r.read(1 << 20)
                if not b: break
                out.write(b)
