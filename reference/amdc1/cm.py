"""AMDC-1 context-mixing core (format v3): nibble-local hashed context slots with check bytes,
12 context models, two match models, three mixers + learned second layer, two APM stages,
32-bit binary arithmetic coder with 16-bit probabilities.

Everything here is integer-only and therefore bit-exact across platforms and with the C++ implementation.
Encoder and decoder share one loop (`_run`) so they cannot drift apart.
"""
from array import array
from operator import mul

M32 = 0xFFFFFFFF
M64 = (1 << 64) - 1
K64 = 0x9E3779B97F4A7C15
K2 = 0xD6E8FEB86659FD93

NM = 12                      # hashed context models
NMM = 2                      # match models
NX = NM + NMM                # model inputs
NIN = 16                     # padded input vector: NX inputs, bias at NX, zero pad
LIM = (250, 250, 250, 120, 60, 60, 60, 60, 120, 120, 120, 120)   # adaptation-count limits
MMIN = (5, 12)               # minimum verified match length per match model
MVER = (32, 48)              # verification depth per match model
INITW = 1228                 # initial mixer weight (1.0 = 2^13)
LR6 = 6                      # first-layer error scale
LRF = 3                      # second-layer learning rate
ACT_FULL = tuple(range(NM))
ACT_LITE = (1, 2, 3, 4, 6, 9)

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


def _clip(v):
    return 2047 if v > 2047 else -2047 if v < -2047 else v


def _sat(v):
    return 32767 if v > 32767 else -32768 if v < -32768 else v


def _run(n, s, data, payload, lite, membits):
    enc = data is not None
    if n == 0:
        return b""
    act = ACT_LITE if lite else ACT_FULL
    nmma = 1 if lite else NMM
    tbfull = min(membits, max(16, (n * 8).bit_length() + 1))
    tbm = [tbfull] * NM
    tbm[0] = min(tbfull, 12)
    tbm[1] = min(tbfull, 19)
    shm = [64 - t for t in tbm]
    maskm = [(1 << t) - 1 for t in tbm]
    mb = min(22, max(12, n.bit_length() + 1))
    msh = 64 - mb
    S = [None] * NM
    for i in act:
        S[i] = array('I', [32768]) * (1 << tbm[i])
    MT = [array('I', [0]) * (1 << mb) for _ in range(nmma)]
    winit = [INITW] * NX + [0] * (NIN - NX)
    W1 = [list(winit) for _ in range(512)]
    W2 = [list(winit) for _ in range(256)]
    W3 = [list(winit) for _ in range(256)]
    WF = [21845] * (256 * 3)
    row = array('H', [min(65535, _squash((j - 16) * 128) * 16) for j in range(33)])
    A = row * 65536
    A2 = row * 65536
    MP = [[49152] * 33 for _ in range(NMM)]
    buf = bytearray(data) if enc else bytearray(n)
    out = bytearray()
    x1, x2, x, pos = 0, M32, 0, 0
    if not enc:
        pl = len(payload)
        x = int.from_bytes(bytes(payload[:4]).ljust(4, b"\0"), "big")
        pos = 4
    st, sq, recip, lim = ST, SQ, RECIP, LIM
    c8 = 0
    wh = pwh = 0
    mptr = [0, 0]
    mlen = [0, 0]
    hbase = [0] * NM
    nbase = [0] * NM
    chk = [0] * NM
    for t in range(n):
        c1 = c8 & 0xFF
        c2 = (c8 >> 8) & 0xFF
        # ---- match models (byte boundary)
        for j in range(nmma):
            if t:
                if mlen[j] and buf[mptr[j]] == c1:
                    if mlen[j] < 65535:
                        mlen[j] += 1
                    mptr[j] += 1
                else:
                    mlen[j] = 0
                if t >= MMIN[j]:
                    if j == 0:
                        hh = ((c8 & 0xFFFFFFFFFF) * K64) & M64
                    else:
                        hh = 0
                        for k in range(1, MMIN[j] + 1):
                            hh = ((hh + buf[t - k] + 1) * K2) & M64
                        hh = (hh * K64) & M64
                    h = hh >> msh
                    if not mlen[j]:
                        cand = MT[j][h]
                        if cand:
                            ln = 0
                            mv = MVER[j]
                            while ln < mv and cand >= ln + 1 and buf[cand - 1 - ln] == buf[t - 1 - ln]:
                                ln += 1
                            if ln >= MMIN[j]:
                                mlen[j] = ln
                                mptr[j] = cand
                    MT[j][h] = t
        pb0 = buf[mptr[0]] if mlen[0] else -1
        pb1 = buf[mptr[1]] if mlen[1] else -1
        pb = (pb0, pb1)
        # ---- contexts
        if s:
            a = buf[t - s] if t >= s else 0
            b2 = buf[t - 2 * s] if t >= 2 * s else 0
            al = buf[t - s - 1] if t >= s + 1 else 0
            cx9 = (a << 8) | c1
            cx10 = (a << 8) | b2
            cx11 = (a << 16) | (al << 8) | c1
        else:
            cx9 = ((wh << 8) | c1) if wh else (c1 | 0x1000000000)
            cx10 = (c8 >> 8) & 0xFFFF
            cx11 = ((pwh << 32) | wh) if wh else ((pwh << 8) | c1 | 0x2000000000000)
        ctxs = (0, c1, c8 & 0xFFFF, c8 & 0xFFFFFF, c8 & 0xFFFFFFFF, c8 & 0xFFFFFFFFFF,
                c8 & 0xFFFFFFFFFFFF, c8, c1 | ((c8 >> 8) & 0xFF00), cx9, cx10, cx11)
        for i in act:
            hbase[i] = ((ctxs[i] + 1 + i * 0x10001) * K64) & M64
        c0 = 1
        byte = buf[t] if enc else 0
        for bp in (7, 6, 5, 4, 3, 2, 1, 0):
            if bp == 7 or bp == 3:
                for i in act:
                    xh = hbase[i] if bp == 7 else (((hbase[i] + c0 * K2) & M64) * K64) & M64
                    nbase[i] = (xh >> shm[i]) & ~15
                    chk[i] = (xh >> (shm[i] - 8)) & 0xFF
            node = c0 if bp >= 4 else ((c0 & ((1 << (3 - bp)) - 1)) | (1 << (3 - bp)))
            xs = [0] * NIN
            cells = []
            for i in act:
                tbl = S[i]
                idx = (nbase[i] + node) & maskm[i]
                c = tbl[idx]
                ck = chk[i]
                if (c >> 24) != ck:
                    c = 32768 | (ck << 24)
                    tbl[idx] = c
                cells.append((i, tbl, idx))
                xs[i] = st[(c & 0xFFFF) >> 4]
            flag = [0, 0]
            eb = [0, 0]
            mi = [0, 0]
            for j in range(nmma):
                if mlen[j] and ((pb[j] | 256) >> (bp + 1)) == c0:
                    eb[j] = (pb[j] >> bp) & 1
                    ml = mlen[j]
                    mi[j] = ml if ml < 16 else min(16 + ((ml - 16) >> 2), 32)
                    sm = st[MP[j][mi[j]] >> 4]
                    xs[NM + j] = sm if eb[j] else -sm
                    flag[j] = 1
            xs[NX] = 256
            wi1 = c0 * 2 + flag[0]
            w1 = W1[wi1]
            w2 = W2[c1]
            d1 = sum(map(mul, w1, xs)) >> 13
            d2 = sum(map(mul, w2, xs)) >> 13
            d1 = _clip(d1)
            d2 = _clip(d2)
            if not lite:
                w3 = W3[c2]
                d3 = _clip(sum(map(mul, w3, xs)) >> 13)
                fb = c0 * 3
                dm = _clip((WF[fb] * d1 + WF[fb + 1] * d2 + WF[fb + 2] * d3) >> 16)
                p3 = sq[d3 + 2048]
            else:
                dm = (d1 + d2) >> 1
                p3 = 0
            p1 = sq[d1 + 2048]
            p2 = sq[d2 + 2048]
            pm = sq[dm + 2048]
            av = dm + 2048
            aw = av & 127
            ai = ((c1 << 8) | c0) * 33 + (av >> 7)
            pa = (A[ai] * (128 - aw) + A[ai + 1] * aw) >> 7
            if not lite:
                h2 = (((((c8 & 0xFFFF) << 8) | c0) * K64) & M64) >> 48
                ai2 = h2 * 33 + (av >> 7)
                pa2 = (A2[ai2] * (128 - aw) + A2[ai2 + 1] * aw) >> 7
                p = (pm * 32 + 3 * pa + 3 * pa2 + 4) >> 3
            else:
                p = (pm * 16 + 3 * pa + 2) >> 2
            if p < 4:
                p = 4
            elif p > 65532:
                p = 65532
            # ---- arithmetic coder
            xmid = x1 + (((x2 - x1) * p) >> 16)
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
            tgt12 = bit << 12
            e1 = (tgt12 - p1) * LR6
            e2 = (tgt12 - p2) * LR6
            W1[wi1] = [_sat(wv + ((xv * e1 + 32768) >> 16)) for wv, xv in zip(w1, xs)]
            W2[c1] = [_sat(wv + ((xv * e2 + 32768) >> 16)) for wv, xv in zip(w2, xs)]
            if not lite:
                e3 = (tgt12 - p3) * LR6
                W3[c2] = [_sat(wv + ((xv * e3 + 32768) >> 16)) for wv, xv in zip(w3, xs)]
                ef = (tgt12 - pm) * LRF
                WF[fb] += (d1 * ef) >> 14
                WF[fb + 1] += (d2 * ef) >> 14
                WF[fb + 2] += (d3 * ef) >> 14
            tgt16 = bit << 16
            for i, tbl, idx in cells:
                cell = tbl[idx]
                pp = cell & 0xFFFF
                cn = (cell >> 16) & 0xFF
                npv = pp + (((tgt16 - pp) * recip[cn]) >> 16)
                tbl[idx] = (cell & 0xFF000000) | npv | ((cn + 1 if cn < lim[i] else cn) << 16)
            tgt, rnd = (65535, 63) if bit else (0, 0)
            A[ai] += (tgt - A[ai] + rnd) >> 6
            A[ai + 1] += (tgt - A[ai + 1] + rnd) >> 6
            if not lite:
                A2[ai2] += (tgt - A2[ai2] + rnd) >> 6
                A2[ai2 + 1] += (tgt - A2[ai2 + 1] + rnd) >> 6
            for j in range(nmma):
                if flag[j]:
                    MP[j][mi[j]] += (((1 if bit == eb[j] else 0) << 16) - MP[j][mi[j]]) >> 6
            c0 = (c0 << 1) | bit
        byte = c0 & 255
        if not enc:
            buf[t] = byte
        c8 = ((c8 << 8) | byte) & M64
        if s == 0:
            if byte >= 128 or 48 <= byte <= 57 or 65 <= byte <= 90 or 97 <= byte <= 122:
                wh = (wh * 0x2F0B4A27 + byte + 1) & M32
            else:
                if wh:
                    pwh = wh
                wh = 0
    if enc:
        out += x1.to_bytes(4, "big")
        return bytes(out)
    return bytes(buf)


def cm_encode(data, s=0, lite=0, membits=22):
    """Compress `data` with context stride `s` (0 = text-oriented contexts)."""
    return _run(len(data), s, bytes(data), None, lite, membits)


def cm_decode(payload, n, s=0, lite=0, membits=22):
    return _run(n, s, None, payload, lite, membits)
