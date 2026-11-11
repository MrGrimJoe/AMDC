"""AMDC-1: adaptive multi-model lossless data compression (reference implementation)."""
from .container import FormatError, decode_file, encode_file

__version__ = "1.3.0"


def compress(data, block_size=16 << 20, level=1, membits=22):
    return encode_file(data, block_size, level, membits)


def decompress(blob, max_output=1 << 32):
    return decode_file(blob, max_output)


__all__ = ["compress", "decompress", "FormatError", "__version__"]
