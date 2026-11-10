"""AMDC-1 context-mixing core: bitwise logistic mixing + binary arithmetic coder.

Everything here is integer-only and therefore bit-exact across platforms.
Encoder and decoder share one loop (`_run`) so they cannot drift apart.
"""
from array import array

M32 = 0xFFFFFFFF
M64 = (1 << 64) - 1
K64 = 0x9E3779B97F4A7C15
KB = 0x9E3779B1

NM = 8                     # hashed context models
NIN = NM + 2               # + match model + bias
LIM = (250, 250, 250, 120, 60, 60, 120, 120)   # adaptation-count limits
LR = 12                    # mixer learning rate (eta = LR/1024), tuned on text/json/float corpus
MINM = 5                   # minimum verified match length

_T = [1, 2, 3, 6, 10, 16, 27, 45, 73, 120, 194, 310, 488, 747, 1101, 1546,
      2047, 2549, 2994, 3348, 3607, 3785, 3901, 3975, 4022, 4050, 4068,
      4079, 4085, 4089, 4092, 4093, 4094]


def _squash(d):
    if d > 2047:
        return 4095
    if d < -2047:
        return 0
    w = d & 127
    i = (d >> 7) + 16
    return (_T[i] * (128 - w) + _T[i + 1] * w + 64) >> 7


SQ = [_squash(d) for d in range(-2048, 2048)]      # index d+2048
ST = [0] * 4096                                    # inverse of squash
_pi = 0
for _x in range(-2047, 2048):
    _v = _squash(_x)
    for _i in range(_pi, _v + 1):
        ST[_i] = _x
    _pi = _v + 1
for _i in range(_pi, 4096):
    ST[_i] = 2047
RECIP = [(65536 * 2) // (2 * n + 3) for n in range(256)]


def _run(n, s, data, payload):
    enc = data is not None
    if n == 0:
        return b""
    tb = min(22, max(16, (n * 8).bit_length() + 1))
    mask = (1 << tb) - 1
    sh = 64 - tb
    mb = min(22, max(12, n.bit_length() + 1))
    msh = 64 - mb
    P = [array('H', [32768]) * (1 << tb) for _ in range(NM)]
    C = [array('B', bytes(1 << tb)) for _ in range(NM)]
    MT = array('I', [0]) * (1 << mb)
    W = [[13107] * (NM + 1) + [0] for _ in range(512)]
    MP = [49152] * 33
    buf = bytearray(data) if enc else bytearray(n)
    out = bytearray()
    x1, x2, x, pos = 0, M32, 0, 0
    if not enc:
        pl = len(payload)
        x = int.from_bytes(bytes(payload[:4]).ljust(4, b"\0"), "big")
        pos = 4
    st, sq, recip, lim = ST, SQ, RECIP, LIM
    c8 = 0
    wh = 0
    mptr = 0
    mlen = 0
    for t in range(n):
        c1 = c8 & 0xFF
        # ---- match model bookkeeping (byte boundary)
        if t:
            if mlen and buf[mptr] == c1:
                if mlen < 65535:
                    mlen += 1
                mptr += 1
            else:
                mlen = 0
            if t >= MINM:
                h = (((c8 & 0xFFFFFFFFFF) * K64) & M64) >> msh
                if not mlen:
                    cand = MT[h]
                    if cand:
                        ln = 0
                        while ln < 32 and cand - 1 - ln >= 0 and \
                                buf[cand - 1 - ln] == buf[t - 1 - ln]:
                            ln += 1
                        if ln >= MINM:
                            mlen = ln
                            mptr = cand
                MT[h] = t
        pb = buf[mptr] if mlen else -1
        # ---- contexts
        if s:
            a = buf[t - s] if t >= s else 0
            b2 = buf[t - 2 * s] if t >= 2 * s else 0
            cx6 = (a << 8) | c1
            cx7 = (a << 8) | b2
        else:
            cx6 = ((wh << 8) | c1) if wh else (c1 | 0x1000000000)
            cx7 = (c8 >> 8) & 0xFFFF
        ctxs = (0, c1, c8 & 0xFFFF, c8 & 0xFFFFFF, c8 & 0xFFFFFFFF,
                c8 & 0xFFFFFFFFFFFF, cx6, cx7)
        bases = [((((cx + 1 + i * 0x10001) * K64) & M64) >> sh)
                 for i, cx in enumerate(ctxs)]
        c0 = 1
        byte = buf[t] if enc else 0
        for bp in (7, 6, 5, 4, 3, 2, 1, 0):
            idx = [(b + c0 * KB) & mask for b in bases]
            xs = [st[Pm[j] >> 4] for Pm, j in zip(P, idx)]
            if mlen and ((pb | 256) >> (bp + 1)) == c0:
                eb = (pb >> bp) & 1
                mi = mlen if mlen < 16 else min(16 + ((mlen - 16) >> 2), 32)
                sm = st[MP[mi] >> 4]
                xs.append(sm if eb else -sm)
                flag = 1
            else:
                xs.append(0)
                flag = 0
            xs.append(256)
            wi = c0 * 2 + flag
            w = W[wi]
            dot = sum(p_ * q_ for p_, q_ in zip(w, xs)) >> 16
            if dot > 2047:
                dot = 2047
            elif dot < -2047:
                dot = -2047
            p = sq[dot + 2048]
            if p < 1:
                p = 1
            # ---- arithmetic coder
            xmid = x1 + ((x2 - x1) >> 12) * p
            if enc:
                bit = (byte >> bp) & 1
            else:
                bit = 1 if x <= xmid else 0
            if bit:
                x2 = xmid
            else:
                x1 = xmid + 1
            while ((x1 ^ x2) & 0xFF000000) == 0:
                if enc:
                    out.append(x2 >> 24)
                else:
                    nb = payload[pos] if pos < pl else 0
                    pos += 1
                    x = ((x << 8) & M32) | nb
                x1 = (x1 << 8) & M32
                x2 = ((x2 << 8) & M32) | 255
            # ---- updates
            err = ((bit << 12) - p) * LR
            W[wi] = [wv + ((xv * err) >> 14) for wv, xv in zip(w, xs)]
            for Pm, Cm, j, lm in zip(P, C, idx, lim):
                nn = Cm[j]
                pp = Pm[j]
                Pm[j] = pp + ((((bit << 16) - pp) * recip[nn]) >> 16)
                if nn < lm:
                    Cm[j] = nn + 1
            if flag:
                MP[mi] += (((1 if bit == eb else 0) << 16) - MP[mi]) >> 6
            c0 = (c0 << 1) | bit
        byte = c0 & 255
        if not enc:
            buf[t] = byte
        c8 = ((c8 << 8) | byte) & M64
        if s == 0:
            if byte >= 128 or 48 <= byte <= 57 or 65 <= byte <= 90 or 97 <= byte <= 122:
                wh = (wh * 0x2F0B4A27 + byte + 1) & M32
            else:
                wh = 0
    if enc:
        out += x1.to_bytes(4, "big")
        return bytes(out)
    return bytes(buf)


def cm_encode(data, s=0):
    """Compress `data` with context stride `s` (0 = text-oriented contexts)."""
    return _run(len(data), s, bytes(data), None)


def cm_decode(payload, n, s=0):
    return _run(n, s, None, payload)
