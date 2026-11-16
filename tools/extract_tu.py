#!/usr/bin/env python3
"""Extract The Sims 3 Title Update 1 (Xbox 360) into the folder Sims3-NX reads as update/.

usage: python extract_tu.py <tu00000004_00000000> <output folder>

The input is the title update package for the USA/Europe disc (media ID 21CDDF46), found in a Title Update
download under 454108E2/000B0000/. It is an STFS (LIVE) package; this reads its file table and copies the files
out, keeping the GameplayRelease folder. Every file is checked against the fingerprint of the update this port
was built for, so a different or damaged package is reported instead of silently producing a broken install.
Only the Python standard library is used.
"""
import hashlib
import os
import struct
import sys

# SHA-256 of each file of Title Update 1 for media ID 21CDDF46.
EXPECTED = {
    "globalpatch.idx": "7e9ecb1955f9fd82f4b09b69eb0ec2cf8874b85f6b2c5fe8fa3a7c6af49ca92b",
    "PrivLoader.xexp": "39081ad344b5b22ed10e7cb432b3785a9c8dbabcca314d24a42ad404af6aa6f0",
    "Sims3.xexp": "e141effed399bdc4d6702c428cc5e7a65a53a16fd8584abbe8b872358de0a952",
    "GameplayRelease/gameplay.package": "452edc0373bd3e0cf8921633be5597d3d6bcd2ab81d9646bfdaf2e4388ba71f7",
    "GameplayRelease/scripts.package": "45668add593b2a014866badeb47d5b7d36c65a1320d02557623ae8015de904bd",
}

# Files of the package Sims3-NX must NOT get: the port's default.xex code is built from the unpatched disc file, and
# with default.xexp present the game refuses to start ("XEX patch image key doesn't match original XEX").
SKIP = {
    "default.xexp": "a5707d9615783b1629efcd912e6eab3e2b3414a0c1dc00e635d31aab9306a86a",
}

BLOCK = 0x1000


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    data = open(sys.argv[1], "rb").read()
    if data[:4] not in (b"LIVE", b"PIRS", b"CON "):
        print("error: %s is not an Xbox 360 package (STFS)" % sys.argv[1])
        return 1
    header = struct.unpack(">I", data[0x340:0x344])[0]
    base = (header + 0xFFF) & ~0xFFF
    table_blocks = struct.unpack("<H", data[0x37C:0x37E])[0]
    table_start = int.from_bytes(data[0x37E:0x381], "little")

    # Data block number -> file offset (read-only packages: one hash block per 0xAA data blocks, plus the
    # level-1 and level-2 hash blocks).
    def offset(block):
        skip = (block // 0xAA + 1)
        if block >= 0xAA:
            skip += block // 0x70E4 + 1
        if block >= 0x70E4:
            skip += 1
        return base + (block + skip) * BLOCK

    entries = []
    for k in range(table_blocks):
        at = offset(table_start + k)
        for e in range(BLOCK // 0x40):
            r = data[at + e * 0x40: at + e * 0x40 + 0x40]
            if not r or r[0] == 0:
                continue
            flags = r[40]
            entries.append({
                "name": r[:flags & 0x3F].decode("latin1"),
                "dir": bool(flags & 0x80),
                "start": int.from_bytes(r[47:50], "little"),
                "parent": struct.unpack(">h", r[50:52])[0],
                "size": struct.unpack(">I", r[52:56])[0],
            })

    def path(index):
        parts = []
        while index >= 0:
            parts.append(entries[index]["name"])
            index = entries[index]["parent"]
        return "/".join(reversed(parts))

    out = sys.argv[2]
    bad = []
    written = set()
    for i, e in enumerate(entries):
        if e["dir"]:
            continue
        rel = path(i)
        if rel in SKIP:
            print("%-36s %10d bytes  skipped (not used by Sims3-NX)" % (rel, e["size"]))
            stale = os.path.join(out, *rel.split("/"))
            if os.path.isfile(stale):
                os.remove(stale)
            continue
        blob = bytearray()
        for b in range(e["start"], e["start"] + (e["size"] + BLOCK - 1) // BLOCK):
            blob += data[offset(b):offset(b) + BLOCK]
        blob = bytes(blob[:e["size"]])
        target = os.path.join(out, *rel.split("/"))
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(target, "wb") as f:
            f.write(blob)
        written.add(rel)
        digest = hashlib.sha256(blob).hexdigest()
        ok = EXPECTED.get(rel) == digest
        if not ok:
            bad.append(rel)
        print("%-36s %10d bytes  %s" % (rel, e["size"], "ok" if ok else "UNEXPECTED"))
    missing = sorted(set(EXPECTED) - written)
    if missing or bad:
        print("\nThis is not Title Update 1 for the USA/Europe disc (media ID 21CDDF46), or it is damaged.")
        if missing:
            print("missing: " + ", ".join(missing))
        return 1
    print("\nDone: copy the contents of %s to sdmc:/switch/Sims3/update/" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
