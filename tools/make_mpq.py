#!/usr/bin/env python3
"""Pack a folder tree into an (uncompressed) MPQ v1 archive that Storm.dll can open.
usage: make_mpq.py <root folder> <out.mpq>     (every file under <root> is stored under its relative path, with backslashes)"""
import os, struct, sys

def build_table():
    t = [0] * 1280; seed = 0x00100001
    for i in range(256):
        idx = i
        for _ in range(5):
            seed = (seed * 125 + 3) % 0x2AAAAB; t1 = (seed & 0xFFFF) << 16
            seed = (seed * 125 + 3) % 0x2AAAAB; t2 = seed & 0xFFFF
            t[idx] = t1 | t2; idx += 0x100
    return t
T = build_table()

def hash_str(s, typ):
    s1, s2 = 0x7FED7FED, 0xEEEEEEEE
    for c in s.upper().replace('/', '\\'):
        c = ord(c)
        s1 = (T[(typ << 8) + c] ^ (s1 + s2)) & 0xFFFFFFFF
        s2 = (c + s1 + s2 + (s2 << 5) + 3) & 0xFFFFFFFF
    return s1

def encrypt(vals, key):
    s2 = 0xEEEEEEEE; out = []
    for v in vals:
        s2 = (s2 + T[0x400 + (key & 0xFF)]) & 0xFFFFFFFF
        out.append((v ^ ((key + s2) & 0xFFFFFFFF)) & 0xFFFFFFFF)
        key = (((~key << 21) + 0x11111111) | (key >> 11)) & 0xFFFFFFFF
        s2 = (v + s2 + (s2 << 5) + 3) & 0xFFFFFFFF
    return out

def main(root, out):
    files = []
    for dp, _, fn in os.walk(root):
        for f in sorted(fn):
            p = os.path.join(dp, f); files.append((os.path.relpath(p, root).replace('/', '\\'), open(p, 'rb').read()))
    files.sort(key=lambda x: x[0].lower())
    files.append(('(listfile)', ('\r\n'.join(n for n, _ in files) + '\r\n').encode()))
    hsize = 16
    while hsize < len(files) * 2: hsize *= 2
    data = b''; block = []; pos = 32
    for name, d in files:
        block.append((pos, len(d), len(d), 0x80000000)); data += d; pos += len(d)
    hashes = [(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFF, 0xFFFF, 0xFFFFFFFF)] * hsize
    for i, (name, _) in enumerate(files):
        h = hash_str(name, 0) & (hsize - 1)
        while hashes[h][4] != 0xFFFFFFFF: h = (h + 1) & (hsize - 1)
        hashes[h] = (hash_str(name, 1), hash_str(name, 2), 0, 0, i)
    hflat = [x for (a, b, l, p, bi) in hashes for x in (a, b, (p << 16) | l, bi)]
    bflat = [x for b in block for x in b]
    hbytes = struct.pack('<%dI' % len(hflat), *encrypt(hflat, hash_str('(hash table)', 3)))
    bbytes = struct.pack('<%dI' % len(bflat), *encrypt(bflat, hash_str('(block table)', 3)))
    hpos = 32 + len(data); bpos = hpos + len(hbytes)
    total = bpos + len(bbytes)
    hdr = b'MPQ\x1a' + struct.pack('<IIHHIIII', 32, total, 0, 3, hpos, bpos, hsize, len(block))
    open(out, 'wb').write(hdr + data + hbytes + bbytes)
    print('wrote %s: %d files, %d bytes' % (out, len(files), total))

if __name__ == '__main__':
    if len(sys.argv) != 3: sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
