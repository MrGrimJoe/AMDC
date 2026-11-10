"""AMDC-1 archive container (spec v1.1 section 9).

Layout (all integers unsigned LEB128 varints unless noted):
    "AMDC" | version u8 (=1) | orig_len | block_size | block records... | CRC32 (u32 LE)
Block record:
    method u8: 0 = raw  -> raw_len | bytes
               1 = CM   -> raw_len | xf u8 | [k | hdr,w,h,ch,ct] | s | payload_len | payload
"""
import zlib

from . import image
from . import transforms as tf
from .cm import cm_decode, cm_encode

MAGIC = b"AMDC"
VERSION = 1
DEFAULT_BLOCK = 1 << 20
MAX_BLOCK = 1 << 28
MAX_STRIDE = 1 << 24
MAX_RATIO = 4096          # decoder rejects CM blocks claiming more than this expansion
SAMPLE = 12288

# (transform, k, context stride s)
CANDS = [(0, 0, 0), (0, 0, 2), (0, 0, 4), (1, 1, 1), (1, 2, 2), (1, 4, 4),
         (2, 1, 1), (2, 4, 4), (3, 2, 0), (3, 4, 0)]


class FormatError(ValueError):
    pass


def put_varint(v):
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def get_varint(buf, pos):
    v = shift = 0
    while True:
        if pos >= len(buf) or shift > 56:
            raise FormatError("bad varint")
        b = buf[pos]
        pos += 1
        v |= (b & 0x7F) << shift
        if not b & 0x80:
            return v, pos
        shift += 7


def _cm_record(raw_len, xf, params, s, payload):
    r = bytearray(b"\x01" + put_varint(raw_len) + bytes([xf]))
    for p in params:
        r += put_varint(p)
    r += put_varint(s) + put_varint(len(payload)) + payload
    return bytes(r)


def _sample(data):
    n = len(data)
    if n <= SAMPLE:
        return data
    c = SAMPLE // 3
    mid = (n - c) // 2
    return data[:c] + data[mid:mid + c] + data[n - c:]


def encode_block(data, level=1):
    n = len(data)
    raw = b"\x00" + put_varint(n) + bytes(data)
    if n == 0:
        return raw
    cands = CANDS[:1] if level == 0 else CANDS
    if level >= 2 or n <= SAMPLE:
        best = None
        for xf, k, s in cands:
            rec = _cm_record(n, xf, (k,) if xf else (), s,
                             cm_encode(tf.apply(xf, k, data), s))
            if best is None or len(rec) < len(best):
                best = rec
    else:
        smp = _sample(data)
        scored = [(len(cm_encode(tf.apply(xf, k, smp), s)), i)
                  for i, (xf, k, s) in enumerate(cands)]
        score, i = min(scored)
        if score >= len(smp):          # sample incompressible: store raw
            return raw
        xf, k, s = cands[i]
        best = _cm_record(n, xf, (k,) if xf else (), s,
                          cm_encode(tf.apply(xf, k, data), s))
    return best if len(best) < len(raw) else raw


def _image_record(data, level):
    info = image.parse_pnm(data)
    if info is None:
        return None
    hdr, w, h, ch = info
    s = min(w, MAX_STRIDE)
    cts = (1, 0) if ch == 3 and level >= 1 else ((1,) if ch == 3 else (0,))
    best = None
    for ct in cts:
        y = image.fwd(data, hdr, w, h, ch, ct)
        rec = _cm_record(len(data), tf.XF_IMAGE, (hdr, w, h, ch, ct), s,
                         cm_encode(y, s))
        if best is None or len(rec) < len(best):
            best = rec
    return best


def encode_file(data, block_size=DEFAULT_BLOCK, level=1):
    data = bytes(data)
    if not 1 <= block_size <= MAX_BLOCK:
        raise ValueError("block_size out of range")

    def wrap(bs, recs):
        return (MAGIC + bytes([VERSION]) + put_varint(len(data)) + put_varint(bs)
                + b"".join(recs) + zlib.crc32(data).to_bytes(4, "little"))

    blocks = [encode_block(data[i:i + block_size], level)
              for i in range(0, len(data), block_size)]
    best = wrap(block_size, blocks)
    if len(data) <= MAX_BLOCK:
        rec = _image_record(data, level)
        if rec is not None:
            raw_rec = b"\x00" + put_varint(len(data)) + data
            cand = wrap(max(len(data), 1), [rec if len(rec) < len(raw_rec) else raw_rec])
            if len(cand) < len(best):
                best = cand
    return best


def decode_file(buf, max_output=1 << 32):
    buf = bytes(buf)
    if len(buf) < 4 + 1 + 1 + 1 + 4 or buf[:4] != MAGIC:
        raise FormatError("not an AMDC archive")
    if buf[4] != VERSION:
        raise FormatError("unsupported version %d" % buf[4])
    end = len(buf) - 4
    pos = 5
    total, pos = get_varint(buf, pos)
    bsize, pos = get_varint(buf, pos)
    if total > max_output:
        raise FormatError("declared size exceeds limit")
    if not 1 <= bsize <= MAX_BLOCK and total:
        raise FormatError("bad block size")
    out = bytearray()
    while len(out) < total:
        if pos >= end:
            raise FormatError("truncated")
        method = buf[pos]
        pos += 1
        n, pos = get_varint(buf, pos)
        if n < 1 or n > bsize or len(out) + n > total:
            raise FormatError("bad block length")
        if method == 0:
            if pos + n > end:
                raise FormatError("truncated raw block")
            out += buf[pos:pos + n]
            pos += n
        elif method == 1:
            if pos >= end:
                raise FormatError("truncated")
            xf = buf[pos]
            pos += 1
            params = []
            cnt = 1 if xf in (1, 2, 3) else 5 if xf == 4 else 0
            if xf > 4:
                raise FormatError("unknown transform")
            for _ in range(cnt):
                v, pos = get_varint(buf, pos)
                params.append(v)
            s, pos = get_varint(buf, pos)
            plen, pos = get_varint(buf, pos)
            if s > MAX_STRIDE or pos + plen > end:
                raise FormatError("bad block header")
            if n > (plen + 8) * MAX_RATIO:
                raise FormatError("implausible expansion ratio")
            if xf in (1, 2) and not 1 <= params[0] <= 255:
                raise FormatError("bad stride")
            if xf == 3 and not 2 <= params[0] <= 255:
                raise FormatError("bad width")
            if xf == 4:
                hdr, w, h, ch, ct = params
                if ch not in (1, 3) or ct not in (0, 1) or (ct and ch != 3) \
                        or w < 1 or h < 1 or hdr + w * h * ch > n:
                    raise FormatError("bad image parameters")
            y = cm_decode(buf[pos:pos + plen], n, s)
            pos += plen
            out += image.inv(y, *params) if xf == 4 else tf.invert(xf, params[0] if params else 0, y)
        else:
            raise FormatError("unknown block method")
    if pos != end:
        raise FormatError("trailing data")
    if zlib.crc32(out) != int.from_bytes(buf[end:], "little"):
        raise FormatError("checksum mismatch")
    return bytes(out)
