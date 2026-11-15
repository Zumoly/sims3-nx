"""Layout-agnostic scan of .idx files for package paths. usage: python -I s3idx.py <root>"""
import os, re, struct, sys

root = sys.argv[1]
for rel, base in (("GameData/global.idx", "GameData"), ("globalRelease.idx", "")):
    d = open(os.path.join(root, rel), "rb").read()
    ver, cnt = struct.unpack_from(">HH", d, 0)
    hits = list(re.finditer(rb"[A-Za-z0-9_./ ]{3,}\.(?:package|bundle|world)", d))
    print(rel, "ver", ver, "count", cnt, "size", len(d), "names", len(hits))
    prev = None
    for m in hits:
        name = m.group().decode()
        lenfield = struct.unpack_from(">I", d, m.start() - 4)[0] if m.start() >= 4 else -1
        gap = None if prev is None else m.start() - 4 - prev
        ok = os.path.exists(os.path.join(root, base, name))
        print("  exists=%s lenOK=%s gap=%s %s" % (ok, lenfield == len(name), gap, name))
        prev = m.end()
    print("  bytes after last name:", len(d) - prev, "first:", d[prev:prev + 32].hex())
