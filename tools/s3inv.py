"""Read-only inventory of a Sims 3 (Xbox 360) dump. usage: python -I s3inv.py <root> <out.json>"""
import collections, json, os, struct, sys, zlib

root, out_path = sys.argv[1], sys.argv[2]
res = {"files": [], "types": {}, "assemblies": [], "case_collisions": [], "non_ascii": [], "long_paths": []}
types = collections.Counter()
type_bytes = collections.Counter()
comps = collections.Counter()


def dbpf_index(f, size):
    f.seek(0)
    h = f.read(96)
    major = struct.unpack_from("<I", h, 4)[0]
    count = struct.unpack_from("<I", h, 0x24)[0]
    isize = struct.unpack_from("<I", h, 0x2C)[0]
    ioff = struct.unpack_from("<I", h, 0x40)[0]
    if not ioff or ioff + isize > size:
        return major, count, ioff, None
    f.seek(ioff)
    d = f.read(isize)
    flags = struct.unpack_from("<I", d, 0)[0]
    p, const = 4, {}
    for bit in range(3):
        if flags & (1 << bit):
            const[bit] = struct.unpack_from("<I", d, p)[0]
            p += 4
    out = []
    for _ in range(count):
        v = {}
        for bit in range(3):
            if bit in const:
                v[bit] = const[bit]
            else:
                v[bit] = struct.unpack_from("<I", d, p)[0]
                p += 4
        lo, off, sz, mem, comp, _u = struct.unpack_from("<IIIIHH", d, p)
        p += 20
        out.append(dict(t=v[0], g=v[1], i=(v[2] << 32) | lo, off=off, sz=sz & 0x7FFFFFFF, mem=mem, comp=comp))
    return major, count, ioff, out


def chunked(f, size):
    f.seek(0)
    head = f.read(1 << 20)
    vals = struct.unpack_from(">%dI" % (len(head) // 4), head)
    info = {"u32_head": ["0x%08X" % x for x in vals[:6]]}
    # walk candidate size table starting at each small offset, stop where offsets reach EOF
    for start in range(2, 8):
        pos, n = 0, 0
        for x in vals[start:]:
            if x == 0 or x > 0x4000000:
                break
            pos += x
            n += 1
            hdr = (start + n) * 4
            if pos + hdr == size:
                info.update(table_start=start, chunks=n, table_end=hdr)
                return info
        info.setdefault("walks", []).append([start, n, pos])
    return info


seen = collections.defaultdict(list)
for dp, dn, fn in os.walk(root):
    for name in fn:
        full = os.path.join(dp, name)
        rel = os.path.relpath(full, root).replace("\\", "/")
        seen[rel.lower()].append(rel)
        if not rel.isascii():
            res["non_ascii"].append(rel)
        if len(rel) > 120:
            res["long_paths"].append(rel)
        size = os.path.getsize(full)
        with open(full, "rb") as f:
            magic = f.read(4)
            rec = dict(path=rel, size=size, magic=magic.hex())
            if magic == b"DBPF":
                major, count, ioff, idx = dbpf_index(f, size)
                rec.update(fmt="DBPF", major=major, entries=count)
                if idx is None:
                    rec["bad_index"] = True
                else:
                    for e in idx:
                        k = "%08X" % e["t"]
                        types[k] += 1
                        type_bytes[k] += e["mem"]
                        comps["%04X" % e["comp"]] += 1
                    if size < 40_000_000 and rel.lower().startswith(("gameplay", "jazz")):
                        rec["res"] = []
                        for e in idx:
                            f.seek(e["off"])
                            raw = f.read(e["sz"])
                            dec = raw
                            if e["comp"] == 0x5A42:
                                try:
                                    dec = zlib.decompress(raw)
                                except Exception:
                                    dec = b""
                            item = dict(t="%08X" % e["t"], i="%016X" % e["i"], sz=e["sz"], mem=e["mem"],
                                        comp="%04X" % e["comp"], head=dec[:16].hex())
                            pos = dec.find(b"MZ")
                            bsjb = dec.find(b"BSJB")
                            if bsjb >= 0:
                                item["clr"] = True
                                ver = dec[bsjb + 16:bsjb + 28].split(b"\0")[0].decode("ascii", "replace")
                                item["clr_ver"] = ver
                                res["assemblies"].append(dict(pkg=rel, inst=item["i"], mz=pos, clr=ver, size=len(dec)))
                            rec["res"].append(item)
            elif magic[:4] == bytes.fromhex("0FF512ED") or magic[:4] == bytes.fromhex("0FF512EE"):
                rec.update(fmt="XCOMPRESS-like", **chunked(f, size))
            else:
                rec["fmt"] = "other"
        res["files"].append(rec)

res["case_collisions"] = [v for v in seen.values() if len(v) > 1]
res["types"] = {k: dict(count=types[k], mem=type_bytes[k]) for k, _ in types.most_common()}
res["compression"] = dict(comps)
json.dump(res, open(out_path, "w"), indent=1)

fm = collections.Counter(r["fmt"] for r in res["files"])
print("formats", dict(fm))
print("compression", dict(comps))
print("collisions", res["case_collisions"], "nonascii", len(res["non_ascii"]), "long", len(res["long_paths"]))
for r in res["files"]:
    if r["fmt"] == "XCOMPRESS-like":
        print(r["path"], r["size"], {k: v for k, v in r.items() if k in ("u32_head", "table_start", "chunks", "table_end", "walks")})
    if r.get("bad_index"):
        print("BADINDEX", r["path"])
    if r["fmt"] == "other" and r["size"]:
        print("other", r["path"], r["size"], r["magic"])
    for x in r.get("res", []):
        print("  ", r["path"], x)
print("top types")
for k, v in list(res["types"].items())[:25]:
    print(k, v)
print("assemblies", len(res["assemblies"]))
