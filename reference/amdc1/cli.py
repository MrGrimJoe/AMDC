import argparse
import sys
import time

from . import FormatError, compress, decompress


def _read(path):
    return sys.stdin.buffer.read() if path == "-" else open(path, "rb").read()


def _write(path, data):
    if path == "-":
        sys.stdout.buffer.write(data)
    else:
        with open(path, "wb") as f:
            f.write(data)


def main(argv=None):
    ap = argparse.ArgumentParser(prog="amdc1", description="AMDC-1 lossless compressor")
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("compress", help="compress a file ('-' = stdin/stdout)")
    c.add_argument("input")
    c.add_argument("output")
    c.add_argument("-b", "--block-size", type=int, default=1 << 20)
    c.add_argument("-l", "--level", type=int, choices=(0, 1, 2), default=1,
                   help="0 fixed model, 1 sampled transform search (default), 2 exhaustive")
    c.add_argument("--verify", action="store_true", help="decode and compare after compressing")
    d = sub.add_parser("decompress", help="decompress an archive")
    d.add_argument("input")
    d.add_argument("output")
    d.add_argument("--max-output", type=int, default=1 << 32)
    a = ap.parse_args(argv)
    try:
        t = time.time()
        if a.cmd == "compress":
            raw = _read(a.input)
            blob = compress(raw, a.block_size, a.level)
            if a.verify and decompress(blob) != raw:
                print("VERIFY FAILED", file=sys.stderr)
                return 2
            _write(a.output, blob)
            print("%d -> %d bytes (%.2f%%) in %.1fs" % (
                len(raw), len(blob), 100.0 * len(blob) / max(len(raw), 1), time.time() - t),
                file=sys.stderr)
        else:
            _write(a.output, decompress(_read(a.input), a.max_output))
    except (FormatError, OSError) as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    return 0
