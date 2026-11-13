"""Independent pure-Python implementation of AMDC-F (LZ4-format blocks + 'AMDF' v1 container).

Written from the format description only (docs/AMDC-F_Fast_Mode.md), byte-by-byte and with no shortcuts, so that
it can serve as a cross-check for the optimised C++ codec in src/fast.cpp: archives written by either side must be
read correctly by the other, and malformed input must be rejected by both. Not fast, not meant to be."""
import struct

M64 = (1 << 64) - 1
MIN_MATCH = 4
MAGIC = b"AMDF"
PAGE_RAW, PAGE_ZERO, PAGE_FILL, PAGE_LZ = 0, 1, 2, 3


class FastError(Exception):
    pass


# ---- block codec -----------------------------------------------------------------------------------
def block_decompress(src, out_len):
    """Strict LZ4 block decode to exactly out_len bytes (same acceptance rules as the C++ decoder)."""
    ip, end = 0, len(src)
    out = bytearray()
    while True:
        if ip >= end:
            raise FastError("truncated block")
        token = src[ip]; ip += 1
        lit = token >> 4
        if lit == 15:
            while True:
                if ip >= end:
                    raise FastError("truncated literal length")
                s = src[ip]; ip += 1
                lit += s
                if lit > out_len:
                    raise FastError("literal length too large")
                if s != 255:
                    break
        if lit > end - ip or len(out) + lit > out_len:
            raise FastError("literal overrun")
        out += src[ip:ip + lit]
        ip += lit
        if ip == end:
            if len(out) != out_len:
                raise FastError("wrong decoded size")
            return bytes(out)
        if end - ip < 2:
            raise FastError("truncated offset")
        offset = src[ip] | (src[ip + 1] << 8)
        ip += 2
        if offset == 0 or offset > len(out):
            raise FastError("bad offset")
        mlen = token & 15
        if mlen == 15:
            while True:
                if ip >= end:
                    raise FastError("truncated match length")
                s = src[ip]; ip += 1
                mlen += s
                if mlen > out_len:
                    raise FastError("match length too large")
                if s != 255:
                    break
        mlen += MIN_MATCH
        if len(out) + mlen > out_len:
            raise FastError("match overrun")
        for _ in range(mlen):
            out.append(out[-offset])


def _put_len(o, n):
    while n >= 255:
        o.append(255); n -= 255
    o.append(n)


def _emit(o, lits, offset, mlen):
    ll = len(lits)
    ml = (mlen - MIN_MATCH) if mlen else 0
    o.append((min(ll, 15) << 4) | min(ml, 15))
    if ll >= 15:
        _put_len(o, ll - 15)
    o += lits
    if mlen:
        o += struct.pack("<H", offset)
        if ml >= 15:
            _put_len(o, ml - 15)


def block_compress(src):
    """Simple greedy LZ77 (hash of 4 bytes, 64 KiB window). Valid LZ4 block; ratio is not the point."""
    n = len(src)
    o = bytearray()
    table = {}
    anchor = ip = 0
    mflimit = n - 12                          # LZ4 rule: last match starts >= 12 bytes before the end
    while ip < mflimit:
        key = bytes(src[ip:ip + 4])
        cand = table.get(key)
        table[key] = ip
        if cand is not None and ip - cand <= 65535:
            m = 4
            limit = n - 5 - ip                # LZ4 rule: last 5 bytes are literals
            while m < limit and src[cand + m] == src[ip + m]:
                m += 1
            _emit(o, src[anchor:ip], ip - cand, m)
            ip += m
            anchor = ip
        else:
            ip += 1
    _emit(o, src[anchor:n], 0, 0)
    return bytes(o)


# ---- hash64 ----------------------------------------------------------------------------------------
def hash64(p):
    n = len(p)
    K1, K2, K0 = 0xFF51AFD7ED558CCD, 0xC4CEB9FE1A85EC53, 0x9E3779B97F4A7C15
    h = [K0 ^ n, (K1 + n) & M64, K2 ^ ((n << 1) & M64), (K0 + 3 * n) & M64]
    i = 0
    while i + 32 <= n:
        for lane in range(4):
            w = int.from_bytes(p[i + 8 * lane:i + 8 * lane + 8], "little")
            x = ((h[lane] ^ w) * K1) & M64
            h[lane] = x ^ (x >> 32)
        i += 32
    rot = lambda v, r: ((v << r) | (v >> (64 - r))) & M64
    x = h[0] ^ rot(h[1], 13) ^ rot(h[2], 29) ^ rot(h[3], 43)
    while i + 8 <= n:
        x = ((x ^ int.from_bytes(p[i:i + 8], "little")) * K1) & M64
        x ^= x >> 32
        i += 8
    x = ((x ^ int.from_bytes(p[i:], "little")) * K2) & M64
    return x ^ (x >> 29)


# ---- pages -----------------------------------------------------------------------------------------
def page_compress(page, max_pct=75):
    """Returns the encoded page, or None when it is not worth compressing (caller keeps it raw)."""
    n = len(page)
    if n and page.count(page[0]) == n:
        return bytes([PAGE_ZERO]) if page[0] == 0 else bytes([PAGE_FILL, page[0]])
    c = block_compress(page)
    if len(c) * 100 > n * max_pct:
        return None
    return bytes([PAGE_LZ]) + c


def page_decompress(enc, n):
    if not enc:
        raise FastError("empty page")
    k = enc[0]
    if k == PAGE_RAW and len(enc) == n + 1:
        return bytes(enc[1:])
    if k == PAGE_ZERO and len(enc) == 1:
        return bytes(n)
    if k == PAGE_FILL and len(enc) == 2:
        return bytes([enc[1]]) * n
    if k == PAGE_LZ:
        return block_decompress(enc[1:], n)
    raise FastError("bad page")


# ---- container -------------------------------------------------------------------------------------
def _put_varint(o, v):
    while v >= 0x80:
        o.append((v & 0x7F) | 0x80); v >>= 7
    o.append(v)


def _get_varint(b, end, pos):
    v = sh = 0
    while True:
        if pos >= end or sh > 56:
            raise FastError("bad varint")
        c = b[pos]; pos += 1
        v |= (c & 0x7F) << sh
        if not c & 0x80:
            return v, pos
        sh += 7


def compress(data, block_size=256 << 10, checksum=True):
    if not 4096 <= block_size <= 1 << 28:
        raise ValueError("block_size out of range")
    o = bytearray(MAGIC)
    o += bytes([1, 1 if checksum else 0])
    _put_varint(o, len(data))
    _put_varint(o, block_size)
    for off in range(0, len(data), block_size):
        blk = data[off:off + block_size]
        if blk.count(blk[0]) == len(blk):
            o += bytes([2, blk[0]])
            continue
        c = block_compress(blk)
        if len(c) < len(blk):
            o.append(1); _put_varint(o, len(c)); o += c
        else:
            o.append(0); o += blk
    if checksum:
        o += struct.pack("<Q", hash64(data))
    return bytes(o)


def decompress(b, max_output=1 << 32):
    inlen = len(b)
    if inlen < 8 or b[:4] != MAGIC:
        raise FastError("not an AMDF archive")
    if b[4] != 1:
        raise FastError("unsupported version")
    chk = b[5] & 1
    if b[5] & ~1:
        raise FastError("unknown flags")
    if chk and inlen < 16:
        raise FastError("truncated")
    end = inlen - (8 if chk else 0)
    total, pos = _get_varint(b, end, 6)
    bsize, pos = _get_varint(b, end, pos)
    if total > max_output:
        raise FastError("declared size exceeds limit")
    if total and not 4096 <= bsize <= 1 << 28:
        raise FastError("bad block size")
    out = bytearray()
    while len(out) < total:
        if pos >= end:
            raise FastError("truncated")
        typ = b[pos]; pos += 1
        ln = min(bsize, total - len(out))
        if typ == 0:
            if ln > end - pos:
                raise FastError("truncated raw block")
            out += b[pos:pos + ln]; pos += ln
        elif typ == 1:
            c, pos = _get_varint(b, end, pos)
            if c > end - pos or c < 1:
                raise FastError("bad block")
            if ln > (c + 16) * 255:
                raise FastError("implausible expansion ratio")
            out += block_decompress(b[pos:pos + c], ln); pos += c
        elif typ == 2:
            if pos >= end:
                raise FastError("truncated")
            out += bytes([b[pos]]) * ln; pos += 1
        else:
            raise FastError("unknown block type")
    if pos != end:
        raise FastError("trailing data")
    if chk and hash64(out) != struct.unpack("<Q", b[end:end + 8])[0]:
        raise FastError("checksum mismatch")
    return bytes(out)
