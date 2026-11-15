"""Print a generated function's PPC comments plus resolved string/call info.
usage: python -I funcview.py <Sims3.bin> <generated_dir> <funcaddr> [maxlines]"""
import glob, os, re, struct, sys

b = open(sys.argv[1], "rb").read()
gen, target, maxl = sys.argv[2], sys.argv[3].upper(), int(sys.argv[4]) if len(sys.argv) > 4 else 220
BASE = 0x88000000
head = "DEFINE_REX_FUNC(sub_%s)" % target
for path in glob.glob(os.path.join(gen, "sims3_recomp.*.cpp")):
    data = open(path, encoding="utf-8", errors="replace").read()
    i = data.find(head)
    if i < 0:
        continue
    j = data.find("DEFINE_REX_FUNC(", i + 10)
    body = data[i:j if j > 0 else None]
    print("file", os.path.basename(path), "bytes", len(body))
    hi = {}
    n = 0
    for line in body.splitlines():
        s = line.strip()
        if not s.startswith("// "):
            continue
        asm = s[3:]
        note = ""
        m = re.match(r"lis r(\d+),(-?\d+)", asm)
        if m:
            hi[m.group(1)] = (int(m.group(2)) << 16) & 0xFFFFFFFF
        m = re.match(r"addi r(\d+),r(\d+),(-?\d+)", asm)
        if m and m.group(2) in hi:
            a = (hi[m.group(2)] + int(m.group(3))) & 0xFFFFFFFF
            note = " ; =%08X" % a
            if BASE <= a < BASE + len(b):
                t = b[a - BASE:a - BASE + 64].split(b"\0")[0]
                if len(t) >= 4 and all(32 <= c < 127 for c in t):
                    note += ' "%s"' % t.decode()
        m = re.match(r"ori r(\d+),r(\d+),(\d+)", asm)
        if m and m.group(2) in hi:
            note = " ; =%08X" % ((hi[m.group(2)] | int(m.group(3))) & 0xFFFFFFFF)
        print("  " + asm + note)
        n += 1
        if n >= maxl:
            print("  ...")
            break
    break
