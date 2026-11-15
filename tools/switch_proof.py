"""Compute, from the image, each byte-table switch's real bound and the first case leaving the function.
usage: python -I switch_proof.py <Sims3.bin>"""
import struct, sys

b = open(sys.argv[1], "rb").read()
BASE = 0x88000000
ENDS = {}
for o in range(0x22B200, 0x22B200 + 0x5C778, 8):
    s, w = struct.unpack_from(">II", b, o)
    if s:
        ENDS[s] = s + ((w >> 8) & 0x3FFFFF) * 4


def word(a):
    return struct.unpack_from(">I", b, a - BASE)[0]


for f in (0x889DF148, 0x88D79100, 0x88DF07B8):
    end = ENDS[f]
    a = f
    while a < end and word(a) != 0x4E800420:  # bctr
        a += 4
    # walk back: lis/addi table, lis/addi base
    regs = {}
    tbl = base = idx = None
    for p in range(a - 40, a):
        pass
    for p in range(a - 44, a, 4):
        w = word(p)
        op = w >> 26
        rd, ra = (w >> 21) & 31, (w >> 16) & 31
        imm = w & 0xFFFF
        imm = imm - 0x10000 if imm & 0x8000 else imm
        if op == 15 and ra == 0:
            regs[rd] = imm << 16
        elif op == 14 and ra in regs:
            regs[rd] = regs[ra] + imm
            if tbl is None:
                tbl = regs[rd] & 0xFFFFFFFF
            else:
                base = regs[rd] & 0xFFFFFFFF
        elif op == 31 and ((w >> 1) & 0x3FF) == 87:  # lbzx
            idx = (w >> 11) & 31
    first_out = None
    for c in range(512):
        t = base + (b[tbl - BASE + c] << 2)
        if t >= end or t < f:
            first_out = c
            break
    # nearest compare on the index before the bctr
    cmps = []
    p = a - 4
    while p >= f and len(cmps) < 3:
        w = word(p)
        if w >> 26 in (10, 11):  # cmpli / cmpi
            cmps.append(("%08X" % p, "r%d" % ((w >> 16) & 31), w & 0xFFFF, "cmpl" if w >> 26 == 10 else "cmp"))
        p -= 4
    print("sub_%08X end %08X bctr %08X index r%s table %08X base %08X first case leaving function: %s" % (f, end, a, idx, tbl, base, first_out))
    print("   compares before dispatch (nearest first):", cmps)
    print("   table head:", b[tbl - BASE:tbl - BASE + 24].hex())
