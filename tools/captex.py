"""Texture fetch constants and shader constants of one captured draw.
usage: python -I captex.py <frame.bin> <draw> [count]"""
import struct, sys

d = open(sys.argv[1], "rb").read()
want = int(sys.argv[2])
count = int(sys.argv[3]) if len(sys.argv) > 3 else 1
p = 8


def u32():
    global p
    v = struct.unpack_from("<I", d, p)[0]
    p += 4
    return v


def f(v):
    return struct.unpack("<f", struct.pack("<I", v))[0]


regs, n = None, 0
while p < len(d):
    tag = u32()
    if tag == 0x53474552:
        c = u32()
        regs = list(struct.unpack_from("<%dI" % c, d, p))
        p += 4 * c
    elif tag == 0x57415244:
        init, pred, base, size = u32(), u32(), u32(), u32()
        vs, ps = struct.unpack_from("<QQ", d, p)
        p += 16
        c = u32()
        delta = struct.unpack_from("<%dI" % (c * 2), d, p)
        p += c * 8
        for i in range(0, c * 2, 2):
            regs[delta[i]] = delta[i + 1]
        if want <= n < want + count:
            print("draw %d vs %016x ps %016x mode %d pitch %d col %08X z %08X blend %08X dc %08X mask %04X colourctl %08X prog %08X misc %08X" % (
                n, vs, ps, regs[0x2208] & 7, regs[0x2000] & 0x3FFF, regs[0x2001], regs[0x2002], regs[0x2201], regs[0x2200],
                regs[0x2104], regs[0x2202], regs[0x2180], regs[0x2181]))
            print("  colour1 %08X colour2 %08X colour3 %08X" % (regs[0x2003], regs[0x2004], regs[0x2005]))
            for t in range(16):
                w = regs[0x4800 + t * 6:0x4800 + t * 6 + 6]
                if (w[0] & 3) == 2:
                    print("  tex %2d base %08X fmt %2d endian %d %dx%d tiled %d clamp %d,%d swz %s w4 %08X w5 %08X" % (
                        t, (w[1] >> 12) << 12, w[1] & 63, (w[1] >> 6) & 3, (w[2] & 0x1FFF) + 1, ((w[2] >> 13) & 0x1FFF) + 1,
                        w[0] >> 31, (w[0] >> 10) & 7, (w[0] >> 13) & 7, "".join("xyzw01?_"[(w[3] >> (1 + 3 * i)) & 7] for i in range(4)), w[4], w[5]))
            print("  ps c0-7", [["%.3f" % f(regs[0x4400 + i * 4 + j]) for j in range(4)] for i in range(0, 8)])
        n += 1
    elif tag == 0x4D454D4F or tag not in (0x53474552, 0x57415244):
        if p + 12 > len(d):
            break
        a, b, e = struct.unpack_from("<III", d, p)
        if e < 0x8000000 and a < 16:
            p += 12 + ((e + 3) & ~3)
        else:
            p += 12
