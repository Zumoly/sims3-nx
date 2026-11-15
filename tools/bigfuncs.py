"""Rank functions in an address range by size and by largest switch in generated code.
usage: python -I bigfuncs.py <Sims3.bin> <generated_dir> <lo> <hi>"""
import glob, os, re, struct, sys

b = open(sys.argv[1], "rb").read()
gen, lo, hi = sys.argv[2], int(sys.argv[3], 16), int(sys.argv[4], 16)
fn = []
for o in range(0x22B200, 0x22B200 + 0x5C778, 8):
    s, w = struct.unpack_from(">II", b, o)
    if s:
        fn.append((((w >> 8) & 0x3FFFFF) * 4, s))
sel = sorted([f for f in fn if lo <= f[1] < hi], reverse=True)
print("functions in range", len(sel), "total bytes", hex(sum(f[0] for f in sel)))
print("largest:", ["%08X:%X" % (a, s) for s, a in sel[:14]])
start = re.compile(r"^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)")
out = []
for path in glob.glob(os.path.join(gen, "sims3_recomp.*.cpp")):
    cur, sw, maxc, n = None, False, 0, 0
    for line in open(path, encoding="utf-8", errors="replace"):
        m = start.match(line)
        if m:
            if cur is not None and maxc > 30:
                out.append((maxc, cur))
            cur = int(m.group(1), 16); maxc = 0; n = 0
        elif "switch (" in line:
            n = 0
        elif line.lstrip().startswith("case "):
            n += 1
            maxc = max(maxc, n)
    if cur is not None and maxc > 30:
        out.append((maxc, cur))
out.sort(reverse=True)
print("largest switches anywhere (cases, func):")
for c, a in out[:25]:
    size = next((s for s, x in fn if x == a), 0)
    print("  %5d %08X size %X%s" % (c, a, size, "  <- in range" if lo <= a < hi else ""))
