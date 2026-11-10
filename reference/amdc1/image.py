"""Lossless 8-bit PGM/PPM path (AMDC-1 section 8).

Pixel-exact AND byte-exact: the header bytes and any trailing bytes are carried
through verbatim, so the original file is reproduced exactly.

Deviation from spec v1.0: YCoCg-R needs a 9-bit residual alphabet, which does not
fit the byte-oriented coder. v1.2 uses the modular "subtract green" transform
(G, R-G mod 256, B-G mod 256) and MED prediction with mod-256 residuals folded to
bytes. A 16-bit-symbol YCoCg-R path is left for a later format version.
"""

WS = b" \t\r\n"


def parse_pnm(d):
    """Return (hdr_len, w, h, channels) for a binary 8-bit P5/P6 file, else None."""
    if len(d) < 3 or d[0] != 0x50 or d[1] not in (0x35, 0x36) or d[2] not in WS:
        return None
    ch = 1 if d[1] == 0x35 else 3
    pos, vals = 2, []
    while len(vals) < 3:
        while pos < len(d) and d[pos] in WS:
            pos += 1
        if pos < len(d) and d[pos] == 0x23:
            while pos < len(d) and d[pos] not in b"\r\n":
                pos += 1
            continue
        st = pos
        while pos < len(d) and 48 <= d[pos] <= 57:
            pos += 1
        if pos == st or pos - st > 9:
            return None
        vals.append(int(d[st:pos]))
    if pos >= len(d) or d[pos] not in WS:
        return None
    pos += 1
    w, h, mv = vals
    if mv != 255 or w < 1 or h < 1 or pos + w * h * ch > len(d):
        return None
    return pos, w, h, ch


def _fold(e):
    return 2 * e if e < 128 else 511 - 2 * e


def _unfold(z):
    return z >> 1 if not z & 1 else (511 - z) >> 1


def _pred(pl, i, x, y, w):
    if y == 0:
        return pl[i - 1] if x else 0
    if x == 0:
        return pl[i - w]
    a, b, c = pl[i - 1], pl[i - w], pl[i - w - 1]
    if c >= max(a, b):
        return min(a, b)
    if c <= min(a, b):
        return max(a, b)
    return a + b - c


def _med_res(pl, w, h):
    res = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            i = y * w + x
            res[i] = _fold((pl[i] - _pred(pl, i, x, y, w)) & 255)
    return res


def _med_inv(res, w, h):
    pl = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            i = y * w + x
            pl[i] = (_pred(pl, i, x, y, w) + _unfold(res[i])) & 255
    return pl


def fwd(d, hdr, w, h, ch, ct):
    n = w * h * ch
    pix = d[hdr:hdr + n]
    planes = [pix[c::ch] for c in range(ch)]
    if ct == 1:
        g = planes[1]
        planes = [bytes((r - q) & 255 for r, q in zip(planes[0], g)), g,
                  bytes((b - q) & 255 for b, q in zip(planes[2], g))]
    out = bytearray(d[:hdr])
    for pl in planes:
        out += _med_res(pl, w, h)
    out += d[hdr + n:]
    return bytes(out)


def inv(y, hdr, w, h, ch, ct):
    n = w * h * ch
    pn = w * h
    planes = [_med_inv(y[hdr + c * pn:hdr + (c + 1) * pn], w, h) for c in range(ch)]
    if ct == 1:
        g = planes[1]
        planes = [bytes((r + q) & 255 for r, q in zip(planes[0], g)), g,
                  bytes((b + q) & 255 for b, q in zip(planes[2], g))]
    pix = bytearray(n)
    for c in range(ch):
        pix[c::ch] = planes[c]
    return bytes(y[:hdr]) + bytes(pix) + bytes(y[hdr + n:])
