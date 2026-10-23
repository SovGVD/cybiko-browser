"""CyOS archives (the "Cy" format of .app files and the flash file system):
building them, with CyOS's own compression (method 2), and reading them.

From Dragonfruit's tools/cybiko_cap.py. The format is described in
file-formats.md of cybiko-docs (https://github.com/SovGVD/cybiko-docs)."""
import struct


class Bits:
    """LSB-first bit reader."""

    def __init__(self, data):
        self.data, self.pos, self.cur, self.left = data, 0, 0, 0

    def bit(self):
        if not self.left:
            self.cur = self.data[self.pos] if self.pos < len(self.data) else 0
            self.pos += 1
            self.left = 8
        b = self.cur & 1
        self.cur >>= 1
        self.left -= 1
        return b

    def bits(self, n):
        v = 0
        for _ in range(n):
            v = v << 1 | self.bit()
        return v


def _length(b):
    if not b.bit():
        return 2 + b.bit()
    if not b.bit():
        return 4 + b.bit()
    if not b.bit():
        return 6 if not b.bit() else 8 + b.bit()
    if not b.bit():
        return 7 if not b.bit() else 10 + b.bit()
    t = b.bits(3)
    if t == 0:
        return 12
    if t == 1:
        return 13
    if t == 2:
        return 0x14 + b.bits(2)
    if t == 3:
        return 16 + b.bit()
    if t == 4:
        return 0x18 + b.bits(3)
    if t == 5:
        return 14
    if t == 6:
        return 0x20 + b.bits(5) if not b.bit() else 18 + b.bit()
    if not b.bit():
        return 15
    if not b.bit():
        return 0x40 + b.bits(6)
    if not b.bit():
        return 0x80 + b.bits(7)
    return 0x100


def _offset(b):
    t = b.bits(3)
    if t == 0:
        return 0x400 + b.bits(10)
    if t == 1:
        return 0x200 + b.bits(9)
    if t == 2:
        return 0x100 + b.bits(8)
    if t == 3:
        return 0x40 + b.bits(6)
    if t == 4:
        return 0x80 + b.bits(7)
    if t == 5:
        return 0x800 + b.bits(11) if not b.bit() else 0x1000 + b.bits(12)
    if t == 6:
        return 0x20 + b.bits(5) if not b.bit() else 0x10 + b.bits(4)
    u = b.bits(2)
    if u == 0:
        return b.bits(1)
    if u == 1:
        return 6 + b.bits(2)
    if u == 2:
        return 2 if b.bit() else 4 + b.bit()
    if not b.bit():
        return 0xC + b.bits(2)
    if not b.bit():
        return 3
    return 10 + b.bit()


def cy_unpack(data, size):
    """CyOS method 2: literals and back references (see the doc)."""
    b, out = Bits(data), bytearray()
    while len(out) < size:
        if not b.bit():
            out.append(b.bits(8))
        else:
            length = _length(b)
            dist = _offset(b) + length
            for _ in range(length):
                out.append(out[-dist] if len(out) >= dist else 0)
    return bytes(out[:size])


def cy_entries(blob):
    """(name, method, contents) for each part of a Cy archive."""
    if blob[:2] != b"Cy":
        raise ValueError("not a CyOS archive (no Cy header)")
    n = struct.unpack_from(">H", blob, 2)[0]
    parts = []
    for i in range(n):
        name_off, off, size = struct.unpack_from(">HII", blob, 6 + 10 * i)
        name = blob[name_off:blob.index(b"\0", name_off)].decode("latin-1")
        raw = blob[off:off + size]
        method = raw[0] if raw else 0
        if method == 0:
            data = raw[1:]
        elif method == 2:
            data = cy_unpack(raw[5:], struct.unpack_from(">I", raw, 1)[0])
        else:
            data = None
        parts.append((name, method, data))
    return parts


# Codes for method 2, as (bits read before the extra bits, base, extra bits);
# they mirror _length and _offset above.
_LENGTH_CODES = [("0", 2, 1), ("10", 4, 1), ("1100", 6, 0), ("1101", 8, 1),
                 ("11100", 7, 0), ("11101", 10, 1), ("1111000", 12, 0),
                 ("1111001", 13, 0), ("1111010", 20, 2), ("1111011", 16, 1),
                 ("1111100", 24, 3), ("1111101", 14, 0), ("11111100", 32, 5),
                 ("11111101", 18, 1), ("11111110", 15, 0), ("111111110", 64, 6),
                 ("1111111110", 128, 7), ("1111111111", 256, 0)]
_OFFSET_CODES = [("000", 1024, 10), ("001", 512, 9), ("010", 256, 8), ("011", 64, 6),
                 ("100", 128, 7), ("1010", 2048, 11), ("1011", 4096, 12),
                 ("1100", 32, 5), ("1101", 16, 4), ("11100", 0, 1), ("11101", 6, 2),
                 ("111101", 2, 0), ("111100", 4, 1), ("111110", 12, 2),
                 ("1111110", 3, 0), ("1111111", 10, 1)]


def _code_table(codes, top):
    table = [None] * (top + 1)
    for prefix, base, extra in codes:
        for v in range(1 << extra):
            bits = prefix + (format(v, "0%db" % extra) if extra else "")
            table[base + v] = bits
    return table


_LENGTHS = _code_table(_LENGTH_CODES, 256)
_OFFSETS = _code_table(_OFFSET_CODES, 8191)
MAX_MATCH = 256


def cy_pack(data):
    """Compress with CyOS method 2 (greedy LZ77). The format could reach
    `length` + 8191 bytes back, but CyOS's decoder keeps only an 8 KB window,
    so references stay within 8191 bytes. They never overlap themselves."""
    bits = []
    heads = {}
    i, n = 0, len(data)

    def remember(pos):
        if pos + 3 <= n:
            heads.setdefault(data[pos:pos + 3], []).append(pos)

    while i < n:
        best_len, best_dist = 0, 0
        for j in reversed(heads.get(data[i:i + 3], ())[-48:]):
            dist = i - j
            if dist > 8191:
                break
            limit = min(MAX_MATCH, dist, n - i)
            if limit <= best_len:
                continue
            k = 0
            while k < limit and data[j + k] == data[i + k]:
                k += 1
            k = min(k, limit)
            if k >= 2 and k > best_len:
                best_len, best_dist = k, dist
                if k == MAX_MATCH:
                    break
        if best_len >= 2:
            bits.append("1" + _LENGTHS[best_len] + _OFFSETS[best_dist - best_len])
            for p in range(i, i + best_len):
                remember(p)
            i += best_len
        else:
            bits.append("0" + format(data[i], "08b"))
            remember(i)
            i += 1
    s = "".join(bits)
    out = bytearray((len(s) + 7) // 8)
    for k, c in enumerate(s):
        if c == "1":
            out[k >> 3] |= 1 << (k & 7)
    return bytes(out)


def cy_archive(parts):
    """Build a Cy archive from (name, contents, compress) tuples.

    Layout: "Cy", u16 count, u16 size of the directory that follows, then
    count x (u16 name offset, u32 data offset, u32 stored size), the names
    (NUL-terminated), and the data. Each part's data starts with its method:
    0 and the bytes, or 2, the u32 unpacked size and the bit stream. All
    numbers are big-endian; offsets count from the start of the archive."""
    names = b"".join(p[0].encode("latin-1") + b"\0" for p in parts)
    stored = []
    for name, data, compress in parts:
        packed = cy_pack(data) if compress else None
        if packed is not None and len(packed) + 5 < len(data) + 1:
            stored.append(b"\x02" + struct.pack(">I", len(data)) + packed)
        else:
            stored.append(b"\x00" + data)
    table = 6 + 10 * len(parts)
    data_start = table + len(names)
    head = bytearray(b"Cy" + struct.pack(">HH", len(parts), data_start - 6))
    name_off, off = table, data_start
    for (name, _, _), blob in zip(parts, stored):
        head += struct.pack(">HII", name_off, off, len(blob))
        name_off += len(name.encode("latin-1")) + 1
        off += len(blob)
    return bytes(head) + names + b"".join(stored)
