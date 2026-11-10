"""Reversible, length-preserving byte transforms (AMDC-1 section 2)."""

XF_ID, XF_DELTA, XF_XOR, XF_SHUFFLE, XF_IMAGE = 0, 1, 2, 3, 4


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


def apply(xf, k, data):
    if xf == XF_ID:
        return bytes(data)
    if xf == XF_DELTA:
        return delta_fwd(data, k)
    if xf == XF_XOR:
        return xor_fwd(data, k)
    if xf == XF_SHUFFLE:
        return shuffle_fwd(data, k)
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
    raise ValueError("unknown transform %d" % xf)
