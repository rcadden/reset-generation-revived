"""Reset Generation .gfx image tools (partial).
Header (40 bytes, little endian u32s):
  0 magic=2, 1 format (0 = raw 32-bit ARGB), 2 width, 3 height, 4 frames, 5..8 ?, 9 data size
Usage: gfx.py topng in.gfx out.png
"""
import sys, struct
from PIL import Image

def load_raw(path):
    d = open(path, 'rb').read()
    magic, fmt, w, h, frames = struct.unpack_from('<5I', d, 0)
    size, = struct.unpack_from('<I', d, 36)
    return fmt, w, h, frames, d

def to_png(src, dst):
    fmt, w, h, frames, d = load_raw(src)
    if fmt != 0:
        raise SystemExit(f'format {fmt} not supported yet')
    img = Image.frombuffer('RGBA', (w, h * frames), d[40:40 + w * h * frames * 4], 'raw', 'BGRA', 0, 1)
    img.save(dst)
    print(src, fmt, w, h, frames)

if __name__ == '__main__':
    if sys.argv[1] == 'topng': to_png(sys.argv[2], sys.argv[3])
