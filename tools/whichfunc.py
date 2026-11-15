"""Name the generated function containing each address, using generated code labels.
usage: python -I whichfunc.py <Sims3.bin> <addr> ..."""
import bisect, struct, sys

b = open(sys.argv[1], "rb").read()
fn = {}
for o in range(0x22B200, 0x22B200 + 0x5C778, 8):
    s, w = struct.unpack_from(">II", b, o)
    if s:
        fn[s] = ((w >> 8) & 0x3FFFFF) * 4
keys = sorted(fn)
for a in (int(x, 16) for x in sys.argv[2:]):
    s = keys[bisect.bisect_right(keys, a) - 1]
    print("%08X in sub_%08X size %X offset %X" % (a, s, fn[s], a - s))
