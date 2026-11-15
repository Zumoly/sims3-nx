"""Render state of each draw in a capture, one line per change of target or pipeline.
usage: python -I capstate.py <frame.bin> [first] [last]"""
import struct, sys

d = open(sys.argv[1], "rb").read()
first = int(sys.argv[2]) if len(sys.argv) > 2 else 0
last = int(sys.argv[3]) if len(sys.argv) > 3 else 1 << 30
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
        if first <= n <= last:
            s = regs[0x2000]
            tl, br = regs[0x2081], regs[0x2082]
            wtl, wbr = regs[0x200E], regs[0x200F]
            print("%3d em%d prim %2d src %d n %5d pitch %4d aa%d col %08X z %08X vte %04X vp %.1f %.1f %.1f %.1f %.3f %.3f off %08X win %08X-%08X sc %d,%d-%d,%d su %08X mask %04X dc %08X vs %08x ps %08x" % (
                n, regs[0x2208] & 7, init & 63, (init >> 6) & 3, init >> 16, s & 0x3FFF, 1 << ((s >> 16) & 3), regs[0x2001], regs[0x2002],
                regs[0x2206], f(regs[0x210F]), f(regs[0x2110]), f(regs[0x2111]), f(regs[0x2112]), f(regs[0x2113]), f(regs[0x2114]),
                regs[0x2080], wtl, wbr, tl & 0x7FFF, (tl >> 16) & 0x7FFF, br & 0x7FFF, (br >> 16) & 0x7FFF, regs[0x2205],
                regs[0x2104] & 0xFFFF, regs[0x2200], vs & 0xFFFFFFFF, ps & 0xFFFFFFFF))
        n += 1
    elif tag == 0x4D454D4F or tag not in (0x53474552, 0x57415244):
        if p + 12 > len(d):
            break
        a, b, e = struct.unpack_from("<III", d, p)
        if e < 0x8000000 and a < 16:
            p += 12 + ((e + 3) & ~3)
        else:
            p += 12
