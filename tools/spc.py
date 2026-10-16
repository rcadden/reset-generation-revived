"""Reset Generation .spc audio -> WAV.  TENTATIVE: the header and table are certain, the
decoding rule (code -> absolute level vs. delta) is not yet confirmed against the engine.
Header (little endian): u16 ?, u16 ?, u16 ?, u16 sample_rate, u16 ?, u16 bits(4),
u32 sample_count, zero padding to 0x22, then a 16-entry int16 table of output levels
(0x22..0x41, ascending: a non-uniform 4-bit quantiser) and 4-bit codes from 0x42.
Each code selects table[code] as the output sample.
Usage: spc.py in.spc out.wav [--low-first]
"""
import struct, sys, wave

def decode(d, low_first=False):
    rate = struct.unpack_from('<H', d, 6)[0]
    bits = struct.unpack_from('<H', d, 10)[0]
    count = struct.unpack_from('<I', d, 12)[0]
    table = struct.unpack_from('<16h', d, 0x22)
    out = []
    for b in d[0x42:]:
        for code in ((b & 15, b >> 4) if low_first else (b >> 4, b & 15)):
            out.append(table[code])
            if len(out) >= count:
                break
    return rate, bits, count, table, out

if __name__ == '__main__':
    d = open(sys.argv[1], 'rb').read()
    rate, bits, count, table, pcm = decode(d, '--low-first' in sys.argv)
    with wave.open(sys.argv[2], 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate)
        w.writeframes(struct.pack('<%dh' % len(pcm), *pcm))
    clipped = sum(1 for x in pcm if abs(x) >= 32767)
    print('%s: %d Hz, %d-bit codes, %d samples (%.1fs), clipped %d, mean %.0f' % (
        sys.argv[1], rate, bits, count, count / rate, clipped, sum(pcm) / max(1, len(pcm))))
