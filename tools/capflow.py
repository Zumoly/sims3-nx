"""One line per draw or resolve of a captured frame, with every texture its pixel shader reads.
usage: python capflow.py <frame.bin> <glsl_dir> [first] [last]"""
import os, re, struct, sys

d = open(sys.argv[1], "rb").read()
glsl = sys.argv[2]
first = int(sys.argv[3]) if len(sys.argv) > 3 else 0
last = int(sys.argv[4]) if len(sys.argv) > 4 else 1 << 30
p = 8


def u32():
    global p
    v = struct.unpack_from("<I", d, p)[0]
    p += 4
    return v


def f(v):
    return struct.unpack("<f", struct.pack("<I", v))[0]


cache = {}


def bindings(ps):
    if ps not in cache:
        path = os.path.join(glsl, "ps_%s.glsl" % ps)
        out = []
        if os.path.exists(path):
            out = [(int(a), b) for a, b in re.findall(r"binding = (\d+)\) uniform sampler(\w+)", open(path).read())]
        cache[ps] = out
    return cache[ps]


def fetch(regs, i):
    w = regs[0x4800 + i * 6:0x4800 + i * 6 + 6]
    fmt = w[1] & 63
    width, height = (w[2] & 0x1FFF) + 1, ((w[2] >> 13) & 0x1FFF) + 1
    sw = "".join("xyzw01__"[(w[3] >> (1 + 3 * c)) & 7] for c in range(4))
    return "t%d:%08X f%d %dx%d e%d s%02X %s cl%X%s" % (i, (w[1] >> 12) << 12, fmt, width, height, (w[1] >> 6) & 3,
                                                 (w[0] >> 2) & 0xFF, sw, (w[0] >> 10) & 7, " T" if w[0] >> 31 else "")


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
            mode = regs[0x2208] & 7
            ps = "%016x" % ps
            ci = regs[0x2001]
            line = "%3d m%d p%X n%-6d ps %s vs %016x rt %03X/%d blend %08X mask %X depth %08X stencil %08X zinfo %08X vp %.0f %.0f %.0f %.0f sc %08X %08X" % (
                n, mode, init & 63, init >> 16, ps, vs, ci & 0xFFF, regs[0x2000] & 0x3FFF, regs[0x2201],
                regs[0x2104] & 15, regs[0x2200], regs[0x210D], regs[0x2002], f(regs[0x210F]), f(regs[0x2110]), f(regs[0x2111]), f(regs[0x2112]), regs[0x2081], regs[0x2082])
            if mode == 6:
                line = "%3d RESOLVE ctl %08X dest %08X pitch %08X info %08X" % (
                    n, regs[0x2318], regs[0x2319], regs[0x231A], regs[0x231B])
            else:
                for i, kind in bindings(ps):
                    line += " | " + fetch(regs, i) + (" cube" if "Cube" in kind else "")
            print(line)
        n += 1
    elif tag == 0x524D454D:
        kind, addr, size = u32(), u32(), u32()
        p += (size + 3) & ~3
    elif tag == 0x50415753:
        p += 12
    else:
        break
print("draws", n)
