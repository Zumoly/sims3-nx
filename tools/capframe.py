"""Outline of a captured frame: runs of draws per render target, and each resolve.
usage: python -I capframe.py <frame.bin>"""
import struct, sys

d = open(sys.argv[1], "rb").read()
p = 8


def u32():
    global p
    v = struct.unpack_from("<I", d, p)[0]
    p += 4
    return v


regs = None
run = None


def flush():
    global run
    if run:
        print("  %4d draws, %6d indices, pitch %4d aa %d edram %03X fmt %2d depth %03X mask %04X ps %s" % (
            run["n"], run["idx"], run["pitch"], run["aa"], run["base"], run["fmt"], run["depth"], run["mask"],
            " ".join(sorted(run["ps"])[:6])))
    run = None


index = 0
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
        mode = regs[0x2208] & 7
        surface, colour, depth = regs[0x2000], regs[0x2001], regs[0x2002]
        pitch, aa = surface & 0x3FFF, 1 << ((surface >> 16) & 3)
        if mode == 6:
            flush()
            control, dest_base, dest_pitch, dest_info = regs[0x2318], regs[0x2319], regs[0x231A], regs[0x231B]
            print("RESOLVE #%d edram %03X pitch %d aa %d -> %08X %dx%d info %08X control %08X (clear %d)" % (
                index, colour & 0xFFF, pitch, aa, dest_base, dest_pitch & 0x3FFF, (dest_pitch >> 16) & 0x3FFF,
                dest_info, control, (control >> 8) & 1))
        else:
            key = (pitch, aa, colour & 0xFFF, (colour >> 16) & 15, depth & 0xFFF, regs[0x2104] & 0xFFFF, mode)
            if not run or run["key"] != key:
                flush()
                run = dict(key=key, n=0, idx=0, pitch=pitch, aa=aa, base=colour & 0xFFF, fmt=(colour >> 16) & 15,
                           depth=depth & 0xFFF, mask=regs[0x2104] & 0xFFFF, ps=set())
            run["n"] += 1
            run["idx"] += init >> 16
            run["ps"].add("%08x" % (ps & 0xFFFFFFFF))
        index += 1
    elif tag == 0x4D454D4F or tag not in (0x53474552, 0x57415244):
        if p + 12 > len(d):
            break
        a, b, e = struct.unpack_from("<III", d, p)
        if e < 0x8000000 and a < 16:
            p += 12 + ((e + 3) & ~3)
        else:
            p += 12
flush()
