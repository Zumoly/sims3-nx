"""Summarise decrypted XEX memory images. usage: python -I imginfo.py <images_dir>"""
import collections, os, re, struct, sys

d = sys.argv[1]
PAT = re.compile(rb"[\x20-\x7e]{5,}")
KEYS = [b"mono", b"Mono", b"mscorlib", b"PrivLoader", b"Sims3.xex", b"Sims3.exe", b"default.xex", b"game:\\", b"d:\\", b"cache:",
        b"XexLoad", b"AOT", b"aot", b".dll", b"JIT", b"0FF512", b"lzx", b"LZX", b"XMemDecompress", b"XMemCreate", b"xcompress",
        b"Xenon", b"S3SA", b"gameplay.package", b"scripts.package", b"simcore", b"Resource.cfg", b"global.idx", b"precomp",
        b"pdb", b"ThreadPool"]
for name in ("default", "PrivLoader", "Sims3"):
    b = open(os.path.join(d, name + ".bin"), "rb").read()
    pe = struct.unpack_from("<I", b, 0x3C)[0]
    nsec = struct.unpack_from("<H", b, pe + 6)[0]
    opt = struct.unpack_from("<H", b, pe + 20)[0]
    print("==", name, "size", hex(len(b)), "sections", nsec)
    for i in range(nsec):
        o = pe + 24 + opt + 40 * i
        n = b[o:o + 8].rstrip(b"\0").decode("ascii", "replace")
        vs, va, rs, ro = struct.unpack_from("<IIII", b, o + 8)
        ch = struct.unpack_from("<I", b, o + 36)[0]
        print("  %-10s va=0x%08X vsize=0x%08X flags=0x%08X" % (n, va, vs, ch))
    strs = [m.group() for m in PAT.finditer(b)]
    print("  strings", len(strs))
    for k in KEYS:
        hit = [s for s in strs if k in s]
        print("  [%s] %d" % (k.decode(), len(hit)), [s[:90].decode() for s in hit[:6]])
