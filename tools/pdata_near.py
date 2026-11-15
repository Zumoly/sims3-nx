"""For each address, show the .pdata function around it, and the words before it.
usage: python -I pdata_near.py <Sims3.bin> <addr> ..."""
import struct, sys

b = open(sys.argv[1], "rb").read()
BASE = 0x88000000
PD, PDS = 0x22B200, 0x5C778
fn = []
for o in range(PD, PD + PDS, 8):
    s, w = struct.unpack_from(">II", b, o)
    if s:
        fn.append((s, ((w >> 8) & 0x3FFFFF) * 4))
fn.sort()
print("pdata functions", len(fn))
for a in (int(x, 16) for x in sys.argv[2:]):
    prev = [f for f in fn if f[0] <= a][-1]
    nxt = [f for f in fn if f[0] > a][0]
    print(hex(a), "prev %08X len %X end %08X | next %08X gap_to_next %X" % (prev[0], prev[1], prev[0] + prev[1], nxt[0], nxt[0] - a))
    print("   ", " ".join("%08X" % struct.unpack_from(">I", b, a - BASE + i)[0] for i in range(-12, 24, 4)))
