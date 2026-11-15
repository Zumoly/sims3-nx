"""List functions in a range with instruction stats, from the image directly.
usage: python -I clusterscan.py <Sims3.bin> <lo> <hi>"""
import struct, sys

b = open(sys.argv[1], "rb").read()
lo, hi = int(sys.argv[2], 16), int(sys.argv[3], 16)
BASE = 0x88000000
fn = []
for o in range(0x22B200, 0x22B200 + 0x5C778, 8):
    s, w = struct.unpack_from(">II", b, o)
    if s and lo <= s < hi:
        fn.append((s, ((w >> 8) & 0x3FFFFF) * 4))
for a, sz in sorted(fn):
    words = struct.unpack_from(">%dI" % (sz // 4), b, a - BASE)
    xors = sum(1 for w in words if w >> 26 == 31 and ((w >> 1) & 0x3FF) in (316, 284))
    xori = sum(1 for w in words if w >> 26 in (26, 27))
    rot = sum(1 for w in words if w >> 26 in (21, 23))
    back = 0
    calls = []
    for i, w in enumerate(words):
        if w >> 26 == 16:
            d = w & 0xFFFC
            d = d - 0x10000 if d & 0x8000 else d
            if d < 0:
                back += 1
        if w >> 26 == 18:
            d = w & 0x3FFFFFC
            d = d - 0x4000000 if d & 0x2000000 else d
            if w & 1:
                calls.append(a + i * 4 + d)
            elif d < 0:
                back += 1
    print("%08X size %5X xor %3d xori %3d rot %3d backbr %3d calls %s" % (a, sz, xors, xori, rot, back, ["%08X" % c for c in calls[:6]]))
