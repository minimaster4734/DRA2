#!/usr/bin/env python3
"""Minimal RVZ -> raw GameCube ISO converter (GC discs: no partitions)."""
import sys, struct, zstandard
U32=struct.Struct('>I')

class LFG:
    K, J = 521, 32
    def __init__(s, seed_bytes, skip):
        b = list(struct.unpack('>17I', seed_bytes))
        for i in range(17, s.K):
            b.append((((b[i-17] << 23) & 0xFFFFFFFF) ^ (b[i-16] >> 9) ^ b[i-1]) & 0xFFFFFFFF)
        b = [((x & 0xFF00FFFF) | ((x >> 2) & 0x00FF0000)) for x in b]
        s.b = b; s.pos = 0
        for _ in range(4): s.fwd()
        s.get(skip)
    def fwd(s):
        b, K, J = s.b, s.K, s.J
        for i in range(J): b[i] ^= b[i+K-J]
        for i in range(J, K): b[i] ^= b[i-J]
    def get(s, n):
        out = bytearray()
        while n > 0:
            if s.pos >= s.K*4:
                s.fwd(); s.pos = 0
            w, o = divmod(s.pos, 4)
            take = min(n, 4-o)
            out += U32.pack(s.b[w])[o:o+take]
            s.pos += take; n -= take
        return bytes(out)

def main(src, dst):
    f = open(src, 'rb')
    h1 = f.read(0x48)
    assert h1[:4] == b'RVZ\x01', 'not RVZ'
    h2size = U32.unpack_from(h1, 12)[0]
    iso_size = struct.unpack_from('>Q', h1, 0x24)[0]
    h2 = f.read(h2size)
    (disc_type, comp, level, chunk) = struct.unpack_from('>IIiI', h2, 0)
    disc_header = h2[0x10:0x90]
    p = 0x90
    n_part, part_off, = struct.unpack_from('>IQ', h2, p); p += 4+8+20+4
    n_raw, raw_off, raw_sz = struct.unpack_from('>IQI', h2, p); p += 16
    n_grp, grp_off, grp_sz = struct.unpack_from('>IQI', h2, p)
    print(f'iso={iso_size} comp={comp} chunk={chunk} parts={n_part} raw={n_raw} groups={n_grp}')
    assert comp == 5, 'only zstd handled'
    assert n_part == 0, 'partitioned (Wii) disc not supported'
    dctx = zstandard.ZstdDecompressor()
    def read_at(o, n): f.seek(o); return f.read(n)
    raws = dctx.decompress(read_at(raw_off, raw_sz), max_output_size=n_raw*0x18) if comp else read_at(raw_off, raw_sz)
    grps = dctx.decompress(read_at(grp_off, grp_sz), max_output_size=n_grp*12)
    out = open(dst, 'wb'); out.truncate(iso_size)
    out.seek(0); out.write(disc_header)
    for r in range(n_raw):
        off, size, gi, ng = struct.unpack_from('>QQII', raws, r*0x18)
        skip = off % 0x8000
        off -= skip; size += skip
        for g in range(ng):
            d_off, d_size, packed = struct.unpack_from('>III', grps, (gi+g)*12)
            compressed = d_size >> 31; d_size &= 0x7FFFFFFF
            pos = off + g*chunk
            length = min(chunk, off+size-pos)
            start_skip = 0
            if d_size == 0:
                data = bytes(length - start_skip)
            else:
                raw = read_at(d_off*4, d_size)
                if compressed:
                    raw = dctx.decompressobj().decompress(raw)
                if packed:
                    res = bytearray(); i = 0; cur = pos + start_skip
                    while i < len(raw):
                        sz = U32.unpack_from(raw, i)[0]; i += 4
                        if sz & 0x80000000:
                            sz &= 0x7FFFFFFF
                            lfg = LFG(raw[i:i+68], cur % 0x8000); i += 68
                            res += lfg.get(sz)
                        else:
                            res += raw[i:i+sz]; i += sz
                        cur += sz
                    data = bytes(res)
                else:
                    data = raw
            out.seek(pos + start_skip); out.write(data[:length-start_skip])
    out.close()
if __name__ == '__main__': main(*sys.argv[1:3])
