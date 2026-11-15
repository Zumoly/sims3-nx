"""Find references to strings in default.bin and print the generated code of the functions using them.
usage: python -I defview.py <default.bin> <generated/default> <regex> [maxlines]"""
import glob, os, re, struct, sys

b = open(sys.argv[1], "rb").read()
gen, pat = sys.argv[2], re.compile(sys.argv[3].encode())
maxl = int(sys.argv[4]) if len(sys.argv) > 4 else 140
BASE, TEXT, TSZ = 0x82000000, 0x40000, 0x675B0
targets = {BASE + m.start(): m.group().decode() for m in re.finditer(rb"[\x20-\x7e]{4,}", b) if pat.search(m.group())}
print({hex(k): v for k, v in targets.items()})
fn = sorted(struct.unpack_from(">I", b, o)[0] for o in range(0x33E00, 0x33E00 + 0x1C18, 8))
fn = [f for f in fn if f]
hi, hit = {}, set()
words = struct.unpack_from(">%dI" % (TSZ // 4), b, TEXT)
for i, w in enumerate(words):
    op = w >> 26
    if op == 15 and ((w >> 16) & 31) == 0:
        hi[(w >> 21) & 31] = (i, (w & 0xFFFF) << 16)
    elif op == 14:
        ra = (w >> 16) & 31
        if ra in hi and i - hi[ra][0] < 24:
            lo = w & 0xFFFF
            lo = lo - 0x10000 if lo & 0x8000 else lo
            a = (hi[ra][1] + lo) & 0xFFFFFFFF
            if a in targets:
                pc = BASE + TEXT + i * 4
                f = max(x for x in fn if x <= pc)
                print("ref %s at %08X in sub_%08X" % (targets[a], pc, f))
                hit.add(f)
for f in sorted(hit):
    head = "DEFINE_REX_FUNC(sub_%08X)" % f
    for path in glob.glob(os.path.join(gen, "sims3_recomp.*.cpp")):
        data = open(path, encoding="utf-8", errors="replace").read()
        i = data.find(head)
        if i < 0:
            continue
        j = data.find("DEFINE_REX_FUNC(", i + 10)
        print("=====", head, os.path.basename(path))
        lines = [l.strip()[3:] for l in data[i:j].splitlines() if l.strip().startswith("// ")]
        hl = {}
        for k, a in enumerate(lines[:maxl]):
            note = ""
            m = re.match(r"lis r(\d+),(-?\d+)", a)
            if m:
                hl[m.group(1)] = (int(m.group(2)) << 16) & 0xFFFFFFFF
            m = re.match(r"addi r(\d+),r(\d+),(-?\d+)", a)
            if m and m.group(2) in hl:
                v = (hl[m.group(2)] + int(m.group(3))) & 0xFFFFFFFF
                note = " ; =%08X" % v
                if BASE <= v < BASE + len(b):
                    t = b[v - BASE:v - BASE + 48].split(b"\0")[0]
                    if len(t) >= 4 and all(32 <= c < 127 for c in t):
                        note += ' "%s"' % t.decode()
            print("  " + a + note)
        break
