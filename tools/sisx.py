"""Minimal Symbian 9 SISX extractor. Usage: sisx.py file.sis outdir"""
import sys, os, struct, zlib

def rlen(b, o):
    l, = struct.unpack_from('<I', b, o); o += 4
    if l & 0x80000000:
        hi, = struct.unpack_from('<I', b, o); o += 4
        l = (l & 0x7fffffff) | (hi << 31)
    return l, o

def pad(l): return (4 - l % 4) % 4

def fields(b):
    o = 0
    while o + 8 <= len(b):
        t, = struct.unpack_from('<I', b, o)
        l, o = rlen(b, o + 4)
        yield t, b[o:o+l]
        o += l + pad(l)

def array(d):
    et, = struct.unpack_from('<I', d, 0); o = 4; out = []
    while o + 4 <= len(d):
        l, o = rlen(d, o)
        out.append((et, d[o:o+l])); o += l + pad(l)
    return out

def decompress(d):
    alg, = struct.unpack_from('<I', d, 0)
    return zlib.decompress(d[12:]) if alg == 1 else d[12:]

def sstr(d): return d.decode('utf-16le', 'replace')

def parse_controller(c, out, depth=0):
    """Collect (dataUnitIndex, fileIndex, target) from a controller."""
    files = []; meta = {}
    data_index = 0
    fl = list(fields(c))
    for t, d in fl:
        if t == 40: data_index, = struct.unpack_from('<I', d, 0)
    for t, d in fl:
        if t == 14:  # Info
            fi = list(fields(d))
            for t2, d2 in fi:
                if t2 == 9: meta['uid'] = hex(struct.unpack('<I', d2)[0])
                if t2 == 1: meta.setdefault('strings', []).append(sstr(d2))
                if t2 == 2:
                    for _, s in array(d2): meta.setdefault('names', []).append(sstr(s))
                if t2 == 4: meta['version'] = struct.unpack('<3i', d2)
        if t == 28:
            install_block(d, data_index, files, out, depth)
    out.append((depth, meta))
    return files

def install_block(d, du, files, out, depth):
    for t2, d2 in fields(d):
        if t2 != 2 or len(d2) < 4: continue
        et, = struct.unpack_from('<I', d2, 0)
        for _, e in array(d2):
            if et == 24:
                fs = list(fields(e))
                target = sstr(fs[0][1])
                idx, = struct.unpack_from('<I', e, len(e) - 4)
                files.append((du, idx, target))
            elif et == 13:
                files.extend(parse_controller(e, out, depth + 1))
            elif et == 26:  # If: expression, installblock, elseifs
                for t3, d3 in fields(e):
                    if t3 == 28: install_block(d3, du, files, out, depth)
                    if t3 == 2:
                        for _, ei in array(d3):
                            for t4, d4 in fields(ei):
                                if t4 == 28: install_block(d4, du, files, out, depth)

def main(path, outdir):
    b = open(path, 'rb').read()
    print('UIDs', [hex(u) for u in struct.unpack_from('<4I', b, 0)])
    t, = struct.unpack_from('<I', b, 16); l, o = rlen(b, 20)
    contents = b[o:o+l]
    ctrl = data = None
    for t, d in fields(contents):
        if t == 3: ctrl = decompress(d)
        if t == 30: data = d
    os.makedirs(outdir, exist_ok=True)
    meta = []
    ctrl_fields = list(fields(ctrl))
    # top-level compressed blob is the Controller field itself
    if ctrl_fields and ctrl_fields[0][0] == 13: ctrl = ctrl_fields[0][1]
    files = parse_controller(ctrl, meta)
    for m in meta: print('controller', m)
    units = []
    for t, d in fields(data):
        for _, du in array(d):
            blobs = []
            for t2, d2 in fields(du):
                for _, fdata in array(d2):
                    for t3, d3 in fields(fdata):
                        if t3 == 3: blobs.append(d3)
            units.append(blobs)
    print('data units', [len(u) for u in units], 'file descs', len(files))
    for du, idx, target in files:
        name = target.replace('!:', '').replace(chr(92), '/').lstrip('/')
        if not name or name.endswith('/'): name += f'unnamed{du}_{idx}'
        try: raw = decompress(units[du][idx])
        except Exception as e: print('fail', target, e); continue
        p = os.path.join(outdir, f'du{du}', name)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, 'wb').write(raw)
        print(f'{du} {idx:4d} {len(raw):10d} {target}')

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
