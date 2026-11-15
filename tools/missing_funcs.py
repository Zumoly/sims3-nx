"""Find code reachable only through data pointers that codegen did not register.
A candidate is a data word pointing into .text, outside every .pdata function, not registered, and
right after a terminator or padding. usage: python -I missing_funcs.py <Sims3.bin> <generated/sims3> [out.toml]"""
import bisect, re, struct, sys

b = open(sys.argv[1], "rb").read()
gen = sys.argv[2]
out = sys.argv[3] if len(sys.argv) > 3 else None
BASE = 0x88000000
TEXT_LO, TEXT_HI = 0x88290000, 0x89260678
SECTIONS = [(0x400, 0x22B024), (0x1270000, 0x13D2CF8)]

ranges = []
for o in range(0x22B200, 0x22B200 + 0x5C778, 8):
    s, w = struct.unpack_from(">II", b, o)
    if s:
        ranges.append((s, s + ((w >> 8) & 0x3FFFFF) * 4))
ranges.sort()
starts = [r[0] for r in ranges]
reg = open(gen + "/sims3_register.cpp", encoding="utf-8", errors="replace").read()
registered = {int(x, 16) for x in re.findall(r"SetFunction\(0x([0-9A-Fa-f]{8}),", reg)}


def word(a):
    return struct.unpack_from(">I", b, a - BASE)[0]


# Extents codegen really covered (tools/func_extents.py). Routines with no .pdata are in here, and
# their jump-table targets must not be declared as functions: that splits the routine.
covered = sorted((int(l.split()[0], 16), int(l.split()[1], 16))
                 for l in open(sys.argv[4]) if l.strip()) if len(sys.argv) > 4 else []
covered_starts = [r[0] for r in covered]
reach = []
furthest = 0
for s, e in covered:
    furthest = max(furthest, e)
    reach.append(furthest)


def in_pdata(a):
    i = bisect.bisect_right(starts, a) - 1
    if i >= 0 and ranges[i][0] <= a < ranges[i][1]:
        return True
    j = bisect.bisect_right(covered_starts, a) - 1
    return j >= 0 and a < reach[j]


def is_terminator(w):
    if w == 0 or w == 0x4E800020 or w == 0x4E800420:
        return True
    return (w >> 26) == 18 and not (w & 1)


found = {}
for lo, hi in SECTIONS:
    for o in range(lo & ~3, hi, 4):
        v = struct.unpack_from(">I", b, o)[0]
        if not (TEXT_LO <= v < TEXT_HI) or v & 3 or v in registered or in_pdata(v):
            continue
        if word(v) == 0 or not is_terminator(word(v - 4)):
            continue
        found.setdefault(v, []).append(BASE + o)


# Function addresses built in code with lis/addi, such as callbacks passed to thread creation.
text = struct.unpack_from(">%dI" % ((TEXT_HI - TEXT_LO) // 4), b, TEXT_LO - BASE)
high = {}
for i, w in enumerate(text):
    op = w >> 26
    if op == 15 and ((w >> 16) & 31) == 0:
        high[(w >> 21) & 31] = (i, (w & 0xFFFF) << 16)
    elif op == 14:
        ra = (w >> 16) & 31
        if ra in high and i - high[ra][0] < 16:
            lo = w & 0xFFFF
            lo = lo - 0x10000 if lo & 0x8000 else lo
            v = (high[ra][1] + lo) & 0xFFFFFFFF
            if (TEXT_LO <= v < TEXT_HI and not v & 3 and v not in registered and not in_pdata(v)
                    and word(v) != 0 and is_terminator(word(v - 4))):
                found.setdefault(v, []).append(TEXT_LO + i * 4)


def extent(a, limit):
    """Walk to the last terminator, following forward branches that stay before limit."""
    p, far = a, a
    while p < limit and p - a < 0x2000:
        w = word(p)
        if w >> 26 == 16:
            d = w & 0xFFFC
            d = d - 0x10000 if d & 0x8000 else d
            if d > 0 and p + d < limit:
                far = max(far, p + d)
        elif w >> 26 == 18 and not (w & 1):
            d = w & 0x3FFFFFC
            d = d - 0x4000000 if d & 0x2000000 else d
            if d > 0 and p + d < limit:
                far = max(far, p + d)
        if is_terminator(w) and p >= far:
            return p + 4 - a
        p += 4
    return p - a


print("registered", len(registered), "pdata", len(ranges), "missing", len(found))
rows = []
for a in sorted(found):
    i = bisect.bisect_left(starts, a)
    limit = starts[i] if i < len(starts) else TEXT_HI
    j = bisect.bisect_left(covered_starts, a)
    if j < len(covered_starts):
        limit = min(limit, covered_starts[j])
    nxt = min((x for x in found if x > a), default=TEXT_HI)
    size = extent(a, min(limit, nxt))
    rows.append((a, size))
sizes = {}
for a, s in rows:
    sizes[s] = sizes.get(s, 0) + 1
print("size histogram", sorted(sizes.items())[:20])
for a, s in rows[:14]:
    print("  %08X size %X refs %s" % (a, s, ["%08X" % x for x in found[a][:2]]))
if out:
    with open(out, "w") as f:
        for a, s in rows:
            f.write('"0x%08X" = { size = 0x%X }\n' % (a, s))
    print("wrote", out, len(rows))
