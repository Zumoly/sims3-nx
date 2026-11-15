"""Per-draw state dump. usage: python -I capdraws.py <frame.bin> [limit]"""
import struct, sys

d = open(sys.argv[1], "rb").read()
limit = int(sys.argv[2]) if len(sys.argv) > 2 else 60
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
        if n < limit:
            tl, br, off = regs[0x2081], regs[0x2082], regs[0x2080]
            t0, t1, t2 = regs[0x4800:0x4803]
            print("%2d mode %d prim %X n %3d pred %d ps %s sc (%d,%d)-(%d,%d) off %08X s0 %08X s1 %08X vte %X vp %.1f %.1f %.1f %.1f blend %08X mask %X tex %08X %dx%d pitch %d bin %08X" % (
                n, regs[0x2208] & 7, init & 63, init >> 16, pred, "%016x" % ps, tl & 0x7FFF, (tl >> 16) & 0x7FFF, br & 0x7FFF, (br >> 16) & 0x7FFF, off,
                regs[0x200E], regs[0x200F], regs[0x2206], f(regs[0x210F]), f(regs[0x2110]), f(regs[0x2111]), f(regs[0x2112]), regs[0x2201], regs[0x2104] & 15,
                (t1 >> 12) << 12, (t2 & 0x1FFF) + 1, ((t2 >> 13) & 0x1FFF) + 1, ((t0 >> 22) & 0x1FF) << 5, regs[0x4800 + 0]))
        n += 1
    elif tag == 0x524D454D:
        kind, addr, size = u32(), u32(), u32()
        p += (size + 3) & ~3
    elif tag == 0x50415753:
        p += 12
    else:
        break
print("draws", n)
