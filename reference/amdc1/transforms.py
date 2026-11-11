"""Reversible, length-preserving byte transforms (AMDC-1 section 2)."""

XF_ID, XF_DELTA, XF_XOR, XF_SHUFFLE, XF_IMAGE, XF_X86 = 0, 1, 2, 3, 4, 5


def delta_fwd(x, k):
    if k >= len(x):
        return bytes(x)
    return bytes(x[:k]) + bytes((a - b) & 255 for a, b in zip(x[k:], x))


def delta_inv(y, k):
    out = bytearray(y)
    for t in range(k, len(out)):
        out[t] = (out[t] + out[t - k]) & 255
    return bytes(out)


def xor_fwd(x, k):
    if k >= len(x):
        return bytes(x)
    return bytes(x[:k]) + bytes(a ^ b for a, b in zip(x[k:], x))


def xor_inv(y, k):
    out = bytearray(y)
    for t in range(k, len(out)):
        out[t] ^= out[t - k]
    return bytes(out)


def shuffle_fwd(x, w):
    """Byte-plane transpose; the `len % w` tail bytes stay in place at the end."""
    m = len(x) - len(x) % w
    out = bytearray()
    for j in range(w):
        out += x[j:m:w]
    out += x[m:]
    return bytes(out)


def shuffle_inv(y, w):
    n = len(y)
    m = n - n % w
    rows = m // w
    out = bytearray(n)
    for j in range(w):
        out[j:m:w] = y[j * rows:(j + 1) * rows]
    out[m:] = y[m:]
    return bytes(out)


def _x86(src, sign):
    """E8/E9 call/jump targets: relative <-> absolute on the low 24 bits (top byte 00/FF is the trigger).

    An opcode that was examined but not converted blocks conversion for the next 3 positions, so a trigger byte
    is never rewritten by a later conversion and both directions make identical decisions."""
    o = bytearray(src)
    n = len(o)
    lf = -10
    i = 0
    while i + 4 < n:
        op = src[i] == 0xE8 or src[i] == 0xE9
        if op and i - lf > 3 and (src[i + 4] == 0x00 or src[i + 4] == 0xFF):
            v = src[i + 1] | (src[i + 2] << 8) | (src[i + 3] << 16)
            a = (v + sign * (i + 5)) & 0xFFFFFF
            o[i + 1] = a & 255
            o[i + 2] = (a >> 8) & 255
            o[i + 3] = a >> 16
            i += 5
        else:
            if op:
                lf = i
            i += 1
    return bytes(o)


def apply(xf, k, data):
    if xf == XF_ID:
        return bytes(data)
    if xf == XF_DELTA:
        return delta_fwd(data, k)
    if xf == XF_XOR:
        return xor_fwd(data, k)
    if xf == XF_SHUFFLE:
        return shuffle_fwd(data, k)
    if xf == XF_X86:
        return _x86(bytes(data), 1)
    raise ValueError("unknown transform %d" % xf)


def invert(xf, k, data):
    if xf == XF_ID:
        return bytes(data)
    if xf == XF_DELTA:
        return delta_inv(data, k)
    if xf == XF_XOR:
        return xor_inv(data, k)
    if xf == XF_SHUFFLE:
        return shuffle_inv(data, k)
    if xf == XF_X86:
        return _x86(bytes(data), -1)
    raise ValueError("unknown transform %d" % xf)
