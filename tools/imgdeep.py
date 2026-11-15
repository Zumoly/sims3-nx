"""Deeper look: imports, exports, wide strings, Mono strings, S3SA entropy.
usage: python -I imgdeep.py <images_dir> <dump_root> <export_table.inc>"""
import collections, math, os, re, struct, sys

d, root, inc = sys.argv[1:4]
tables = {"xboxkrnl.exe": {}, "xam.xex": {}}
for lib, path in (("xboxkrnl.exe", inc), ("xam.xex", inc.replace("xboxkrnl", "xam").replace("xam_", "xam_"))):
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.search(r"XE_EXPORT\(\s*(\w+)\s*,\s*0x([0-9A-Fa-f]+)\s*,\s*(\w+)", line)
        if m:
            tables[lib][int(m.group(2), 16)] = m.group(3)

for name in ("default", "PrivLoader", "Sims3"):
    b = open(os.path.join(d, name + ".bin"), "rb").read()
    x = open(os.path.join(root, name + ".xex"), "rb").read()
    print("==", name)
    # import thunks: records in .idata are 0x7D-type >> pairs; use XEX header library table
    n = struct.unpack_from(">I", x, 20)[0]
    off = None
    for i in range(n):
        k, v = struct.unpack_from(">II", x, 24 + 8 * i)
        if k == 0x103FF:
            off = v
    size, strsz, cnt = struct.unpack_from(">III", x, off)
    pos = off + 12
    libnames = x[pos:pos + strsz].split(b"\0")
    print("  libs", [l.decode() for l in libnames if l])
    p = pos + strsz
    imp = []
    for _ in range(cnt):
        rs = struct.unpack_from(">I", x, p)[0]
        ni, ic = struct.unpack_from(">HH", x, p + 0x24)
        libn = [l.decode() for l in libnames if l][ni]
        for i in range(ic):
            va = struct.unpack_from(">I", x, p + 0x28 + 4 * i)[0]
            rec = struct.unpack_from(">I", b, va - (0x88000000 if name == "Sims3" else 0x82000000))[0]
            if (rec >> 24) == 0:
                imp.append((libn, rec & 0xFFFF))
        p += rs
    print("  imports", len(imp))
    want = [o for o in imp]
    picks = [tables.get(l, {}).get(o, l + ":" + hex(o)) for l, o in want]
    print("  key:", [s for s in picks if re.search(r"Xex|Exception|Loader|Launch|MmAlloc|MmMap|NtAllocate|NtProtect|Compress|Xam.*Title|ExCreateThread|KeSetAffin|Dvd|Cache", s)])
    # wide strings
    wide = re.findall(rb"(?:[\x20-\x7e]\x00){5,}|(?:\x00[\x20-\x7e]){5,}", b)
    w = [s.replace(b"\0", b"").decode() for s in wide]
    print("  wide", len(w), [s[:80] for s in w if re.search(r"xex|game:|cache|\.package|\.exe|\.dll", s, re.I)][:15])
    ascii_ = [m.group().decode() for m in re.finditer(rb"[\x20-\x7e]{4,}", b)]
    print("  paths:", [s[:80] for s in ascii_ if re.search(r"\.xex|\.exe\b|:\\|\\Device|\\\?\?", s)][:15])
    if name == "Sims3":
        pe = struct.unpack_from("<I", b, 0x3C)[0]
        opt = pe + 24
        magic = struct.unpack_from("<H", b, opt)[0]
        ed_rva = struct.unpack_from("<I", b, opt + 96)[0]
        print("  export dir rva", hex(ed_rva))
        if False:  # .edata is not a standard PE export directory on this title
            nf, nn, af, nnp = struct.unpack_from(">IIII", b, ed_rva + 20)
            print("  exports funcs", nf, "names", nn)
            np_ = struct.unpack_from(">I", b, ed_rva + 32)[0]
            lst = []
            for i in range(nn):
                r = struct.unpack_from(">I", b, np_ + 4 * i)[0]
                lst.append(b[r:b.index(b"\0", r)].decode("ascii", "replace"))
            print("  sample", lst[:40])
        for kw in ("aot", "AOT", "interp", "jit", "mini_", "ppc", "trampoline", "ldftn", "Mono 1", "assembly_load", "mono_", "execution engine", "native code", "ExecutionEngine", "S3SA", "Decrypt", "decrypt", "signature"):
            h = [s for s in ascii_ if kw in s and "Mono/" not in s]
            print("  <%s> %d %s" % (kw, len(h), [s[:70] for s in h[:8]]))

ent = lambda q: -sum(c / len(q) * math.log2(c / len(q)) for c in collections.Counter(q).values())
p = os.path.join(root, "GameplayRelease", "scripts.package")
f = open(p, "rb").read()
h = f[:96]
cnt, isz, ioff = struct.unpack_from("<I", h, 0x24)[0], struct.unpack_from("<I", h, 0x2C)[0], struct.unpack_from("<I", h, 0x40)[0]
idx = f[ioff:ioff + isz]
fl = struct.unpack_from("<I", idx, 0)[0]
q = 4 + 4 * bin(fl & 7).count("1")
const_t = 0
print("flags", hex(fl))
sizes = []
for i in range(cnt):
    vals = []
    for bit in range(3):
        if not fl & (1 << bit):
            vals.append(struct.unpack_from("<I", idx, q)[0]); q += 4
    lo, off, sz, mem, comp, u = struct.unpack_from("<IIIIHH", idx, q); q += 20
    sz &= 0x7FFFFFFF
    blob = f[off:off + sz]
    print(" res", i, "size", sz, "comp", hex(comp), "H(all)=%.3f" % ent(blob), "H(head4k)=%.3f" % ent(blob[:4096]), "H(tail4k)=%.3f" % ent(blob[-4096:]), blob[:32].hex())
    print("   ascii", [m.group().decode()[:50] for m in re.finditer(rb"[\x20-\x7e]{8,}", blob)][:10])
    print("   tail", blob[-48:].hex())
