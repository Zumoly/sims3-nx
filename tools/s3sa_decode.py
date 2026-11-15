"""Decode S3SA (type 0x073FAA07) signed assemblies from the dump's DBPF packages.
Logic follows sub_888824D8 in Sims3.xex. usage: python -I s3sa_decode.py <dump_root> <out_dir>"""
import os, re, struct, sys

root, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
PKGS = ["Gameplay/simcore.package", "Gameplay/simcore_legacy.package", "GameplayRelease/automation.package",
        "GameplayRelease/gameplay.package", "GameplayRelease/scripts.package"]


def refpack(d):
    flags = d[0]
    p = 2
    if flags & 0x80:
        size = int.from_bytes(d[p:p + 4], "big"); p += 4
    else:
        size = int.from_bytes(d[p:p + 3], "big"); p += 3
    o = bytearray()
    while p < len(d):
        c = d[p]; p += 1
        if c >= 0xFC:
            n = c & 3
            o += d[p:p + n]
            break
        if c >= 0xE0:
            n = ((c & 0x1F) << 2) + 4
            o += d[p:p + n]; p += n
            continue
        if c < 0x80:
            b1 = d[p]; p += 1
            lit = c & 3; cnt = ((c & 0x1C) >> 2) + 3; dist = ((c & 0x60) << 3) + b1 + 1
        elif c < 0xC0:
            b1, b2 = d[p], d[p + 1]; p += 2
            lit = b1 >> 6; cnt = (c & 0x3F) + 4; dist = ((b1 & 0x3F) << 8) + b2 + 1
        else:
            b1, b2, b3 = d[p], d[p + 1], d[p + 2]; p += 3
            lit = c & 3; cnt = ((c & 0x0C) << 6) + b3 + 5; dist = ((c & 0x10) << 12) + (b1 << 8) + b2 + 1
        o += d[p:p + lit]; p += lit
        for _ in range(cnt):
            o.append(o[-dist])
    return bytes(o), size


def decode(blob):
    if blob[0] != 1:
        return None, "version %d" % blob[0]
    for name, u16, u64 in (("LE", "<H", "<Q"), ("BE", ">H", ">Q")):
        n = struct.unpack_from(u16, blob, 69)[0]
        tsize = n * 8
        if n == 0 or 71 + tsize > len(blob):
            continue
        table = blob[71:71 + tsize]
        ents = struct.unpack_from(u64[0] + "%dQ" % n, table)
        stored = sum(1 for e in ents if not e & 1)
        if 71 + tsize + stored * 512 != len(blob):
            continue
        idx = (sum(ents) & 0xFFFFFFFF) & (tsize - 1)
        p = 71 + tsize
        o = bytearray(n * 512)
        for c, e in enumerate(ents):
            if e & 1:
                continue
            base = c * 512
            for i in range(512):
                v = blob[p + i]
                o[base + i] = v ^ table[idx]
                idx = (v + idx) % tsize
            p += 512
        return bytes(o), "%s chunks=%d stored=%d table=%d" % (name, n, stored, tsize)
    return None, "no layout matched, u16le=%d u16be=%d size=%d" % (
        struct.unpack_from("<H", blob, 69)[0], struct.unpack_from(">H", blob, 69)[0], len(blob))


total = ok = 0
for rel in PKGS:
    f = open(os.path.join(root, rel), "rb").read()
    count, isz, ioff = (struct.unpack_from("<I", f, x)[0] for x in (0x24, 0x2C, 0x40))
    idx = f[ioff:ioff + isz]
    fl = struct.unpack_from("<I", idx, 0)[0]
    q = 4
    const = {}
    for bit in range(3):
        if fl & (1 << bit):
            const[bit] = struct.unpack_from("<I", idx, q)[0]; q += 4
    for i in range(count):
        v = {}
        for bit in range(3):
            if bit in const:
                v[bit] = const[bit]
            else:
                v[bit] = struct.unpack_from("<I", idx, q)[0]; q += 4
        lo, off, sz, mem, comp, _ = struct.unpack_from("<IIIIHH", idx, q); q += 20
        sz &= 0x7FFFFFFF
        if v[0] != 0x073FAA07:
            continue
        total += 1
        blob = f[off:off + sz]
        if comp == 0xFFFF:
            blob, want = refpack(blob)
            if len(blob) != want:
                print("  refpack size mismatch", len(blob), want)
        dec, info = decode(blob)
        inst = (v[2] << 32) | lo
        if dec is None:
            print(rel, "%016X FAILED %s" % (inst, info))
            continue
        mz, bsjb = dec[:2] == b"MZ", dec.find(b"BSJB")
        names = re.findall(rb"[A-Za-z0-9_.]{3,48}\.dll", dec)
        name = names[0].decode() if names else "%016X.dll" % inst
        ver = dec[bsjb + 16:bsjb + 40].split(b"\0")[0].decode("ascii", "replace") if bsjb >= 0 else "-"
        print("%-36s %016X %-34s MZ=%s BSJB=%s clr=%s size=%d names=%s" % (rel, inst, info, mz, bsjb >= 0, ver, len(dec), sorted(set(n.decode() for n in names))[:5]))
        if mz and bsjb >= 0:
            ok += 1
            open(os.path.join(out, "%016X.bin" % inst), "wb").write(dec)
print("decoded", ok, "of", total)
