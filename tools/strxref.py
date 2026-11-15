"""Find code that references strings, by scanning lis/addi pairs in .text.
usage: python -I strxref.py <Sims3.bin> [regex ...]
Also prints samples of ScriptCore-style strings."""
import bisect, re, struct, sys

b = open(sys.argv[1], "rb").read()
BASE = 0x88000000
TEXT, TSZ = 0x290000, 0xFD0678
fn = sorted(struct.unpack_from(">I", b, o)[0] for o in range(0x22B200, 0x22B200 + 0x5C778, 8))
fn = [f for f in fn if f]
owner = lambda a: fn[bisect.bisect_right(fn, a) - 1]

# build address -> referencing code map
hi = {}
refs = {}
words = struct.unpack_from(">%dI" % (TSZ // 4), b, TEXT)
for i, w in enumerate(words):
    op = w >> 26
    if op == 15 and ((w >> 16) & 31) == 0:  # lis
        hi[(w >> 21) & 31] = (i, (w & 0xFFFF) << 16)
    elif op in (14, 32, 34, 48, 50, 36, 38, 44):  # addi / lwz / lbz / lfs...
        ra = (w >> 16) & 31
        if ra in hi and i - hi[ra][0] < 16:
            lo = w & 0xFFFF
            lo = lo - 0x10000 if lo & 0x8000 else lo
            addr = (hi[ra][1] + lo) & 0xFFFFFFFF
            refs.setdefault(addr, []).append(BASE + TEXT + i * 4)

strs = [(m.start(), m.group().decode()) for m in re.finditer(rb"[\x20-\x7e]{6,}", b)]
for pat in sys.argv[2:]:
    r = re.compile(pat)
    hits = [(o, s) for o, s in strs if r.search(s)]
    print("##", pat, "hits", len(hits))
    for o, s in hits[:14]:
        rs = refs.get(BASE + o, [])
        print("  %08X %-70s refs=%d %s" % (BASE + o, s[:70], len(rs), ["%08X" % owner(a) for a in rs[:3]]))
