"""Record what codegen covered: each function's start and end, from a known-good generated tree.
Each asm comment is one instruction and code is contiguous from the start, so end = start + 4 * count.
usage: python3 func_extents.py <generated/sims3> <out.txt>"""
import glob, os, re, sys

head = re.compile(r"^DEFINE_REX_FUNC\((?:s3dll_)?(\w+)\)")
asm = re.compile(r"^\s*// [a-z]")
reg = open(os.path.join(sys.argv[1], "sims3_register.cpp"), encoding="utf-8", errors="replace").read()
addr = {n: int(a, 16) for a, n in re.findall(r"SetFunction\(0x([0-9A-Fa-f]{8}), (?:s3dll_)?(\w+)\)", reg)}
rows = []
for path in glob.glob(os.path.join(sys.argv[1], "sims3_recomp.*.cpp")):
    name, count = None, 0
    for line in open(path, encoding="utf-8", errors="replace"):
        m = head.match(line)
        if m:
            if name in addr:
                rows.append((addr[name], addr[name] + count * 4))
            name, count = m.group(1), 0
        elif asm.match(line) and "ERROR" not in line:
            count += 1
    if name in addr:
        rows.append((addr[name], addr[name] + count * 4))
rows.sort()
with open(sys.argv[2], "w") as f:
    for s, e in rows:
        f.write("%08X %08X\n" % (s, e))
print("functions", len(rows), "largest", max(e - s for s, e in rows))
