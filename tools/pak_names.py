"""Find a pak's name list and test hash functions against entry hashes."""
import sys, struct, zlib, binascii
sys.path.insert(0, __import__('os').path.dirname(__file__))
from pak import read_pak

def parse_namelist(d):
    try:
        n, = struct.unpack_from('<I', d, 0); o = 4; names = []
        for _ in range(n):
            l, = struct.unpack_from('<H', d, o); o += 2
            names.append(d[o:o+l].decode('latin1')); o += l
        if o == len(d) and n > 0: return names
    except Exception: pass
    return None

def hashes(s):
    out = {}
    for variant, t in (('raw', s), ('lower', s.lower()), ('upper', s.upper()),
                       ('fwd', s.replace(chr(92), '/')), ('fwdlower', s.replace(chr(92), '/').lower())):
        b = t.encode('latin1')
        out[variant + ':crc32'] = binascii.crc32(b) & 0xffffffff
        h = 0x811c9dc5
        for c in b: h = ((h ^ c) * 0x01000193) & 0xffffffff
        out[variant + ':fnv1a'] = h
        h = 0x811c9dc5
        for c in b: h = ((h * 0x01000193) & 0xffffffff) ^ c
        out[variant + ':fnv1'] = h
        h = 5381
        for c in b: h = (h * 33 + c) & 0xffffffff
        out[variant + ':djb2'] = h
        h = 5381
        for c in b: h = ((h * 33) ^ c) & 0xffffffff
        out[variant + ':djb2x'] = h
        h = 0
        for c in b: h = (c + (h << 6) + (h << 16) - h) & 0xffffffff
        out[variant + ':sdbm'] = h
        h = 0
        for c in b: h = (h * 31 + c) & 0xffffffff
        out[variant + ':java'] = h
        h = 0
        for c in b:
            h = ((h << 4) + c) & 0xffffffff; g = h & 0xf0000000
            if g: h ^= g >> 24
            h &= ~g & 0xffffffff
        out[variant + ':elf'] = h
    return out

ents = read_pak(sys.argv[1])
hs = {e[1] for e in ents}
for e in ents:
    names = parse_namelist(e[6])
    if names:
        print('namelist entry', e[0], len(names), names[:3])
        hits = {}
        for nm in names:
            for k, v in hashes(nm).items():
                if v in hs: hits[k] = hits.get(k, 0) + 1
        print('hits', hits)
