"""Reset Generation .pak extractor.
Format (little endian):
  u32 header_size, u32 count
  count x { u32 name_hash, u32 stored_size, u32 raw_size, u8 compressed, u32 offset }
  data; compressed entries are zlib streams.
Usage: pak.py list|extract file.pak [outdir]
"""
import sys, os, struct, zlib, collections

def read_pak(path):
    b = open(path, 'rb').read()
    hdr, count = struct.unpack_from('<II', b, 0)
    out = []
    for i in range(count):
        h, cs, us, fl, off = struct.unpack_from('<IIIBI', b, 8 + i * 17)
        raw = b[off:off + cs]
        if fl == 1:
            try: raw = zlib.decompress(raw)
            except Exception as e: raw = b'' ; print('zlib fail', hex(h), e)
        out.append((i, h, cs, us, fl, off, raw))
    return out

def sniff(d):
    if d[:8] == b'\x89PNG\r\n\x1a\n': return 'png'
    if d[:4] == b'RIFF': return 'wav'
    if d[:4] == b'OggS': return 'ogg'
    if d[:3] == b'ID3' or d[:2] == b'\xff\xfb': return 'mp3'
    if d[:2] == b'\xff\xd8': return 'jpg'
    if d[:5] == b'<?xml': return 'xml'
    if d[:4] in (b'MThd',): return 'mid'
    if d[:4] == b'IMPM': return 'it'
    if d[:17] == b'Extended Module: ': return 'xm'
    if len(d) > 1080 and d[1080:1084] in (b'M.K.', b'4CHN', b'8CHN'): return 'mod'
    if d[:4].isalpha() and d[:4].isascii(): return 'tag_' + d[:4].decode()
    try:
        t = d[:200].decode('utf-8')
        if all(c.isprintable() or c in '\r\n\t' for c in t): return 'txt'
    except Exception: pass
    return 'bin'

def namelist(ents):
    """The last entry of each pak lists the other entries' paths, in entry order."""
    d = ents[-1][6]
    try:
        n, = struct.unpack_from('<I', d, 0); o = 4; names = []
        for _ in range(n):
            l, = struct.unpack_from('<H', d, o); o += 2
            names.append(d[o:o + l].decode('latin1')); o += l
        if o == len(d) and n == len(ents) - 1:
            return names + ['_namelist.bin']
    except Exception:
        pass
    return None

if __name__ == '__main__':
    cmd, path = sys.argv[1], sys.argv[2]
    ents = read_pak(path)
    kinds = collections.Counter()
    if cmd == 'extract':
        od = sys.argv[3]; os.makedirs(od, exist_ok=True)
    names = namelist(ents)
    for i, h, cs, us, fl, off, raw in ents:
        k = sniff(raw); kinds[k] += 1
        name = names[i].replace(chr(92), '/') if names else f'{i:04d}_{h:08x}.{k}'
        if cmd == 'list': print(f'{i:5d} {h:08x} {us:9d} {fl} {k:10s} {name}')
        else:
            p = os.path.join(od, name)
            os.makedirs(os.path.dirname(p) or od, exist_ok=True)
            open(p, 'wb').write(raw)
    print(os.path.basename(path), len(ents), dict(kinds))
