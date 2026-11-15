"""Summarise a captured frame. usage: python -I capview.py <frame.bin> [draw_limit]"""
import collections, struct, sys

d = open(sys.argv[1], "rb").read()
limit = int(sys.argv[2]) if len(sys.argv) > 2 else 12
p = 0


def u32():
    global p
    v = struct.unpack_from("<I", d, p)[0]
    p += 4
    return v


magic, version = u32(), u32()
print("magic %08X version %d size %d" % (magic, version, len(d)))
regs = {}
draws, mem, kinds = [], [], collections.Counter()
swap = None
while p < len(d):
    tag = u32()
    if tag == 0x53474552:  # REGS
        n = u32()
        vals = struct.unpack_from("<%dI" % n, d, p)
        p += 4 * n
        regs = dict(enumerate(vals))
    elif tag == 0x57415244:  # DRAW
        init, pred, base, size = u32(), u32(), u32(), u32()
        vs = struct.unpack_from("<Q", d, p)[0]
        ps = struct.unpack_from("<Q", d, p + 8)[0]
        p += 16
        n = u32()
        delta = struct.unpack_from("<%dI" % (n * 2), d, p)
        p += n * 8
        for i in range(0, n * 2, 2):
            regs[delta[i]] = delta[i + 1]
        draws.append(dict(init=init, pred=pred, base=base, size=size, vs=vs, ps=ps, n=n,
                          mode=regs.get(0x2208, 0) & 7, surface=regs.get(0x2000, 0), color=regs.get(0x2001, 0)))
    elif tag == 0x524D454D:  # MEMR
        kind, addr, size = u32(), u32(), u32()
        mem.append((kind, addr, size))
        kinds[kind] += size
        p += (size + 3) & ~3
    elif tag == 0x50415753:  # SWAP
        swap = (u32(), u32(), u32())
    else:
        print("unknown tag %08X at %d" % (tag, p))
        break
print("parsed to", p, "of", len(d), "swap", swap and tuple(hex(x) for x in swap))
print("draws", len(draws), "resolves", sum(x["mode"] == 6 for x in draws))
print("memory ranges", len(mem), "bytes by kind (0 index, 1 vertex, 2 texture)", dict(kinds))
print("shader pairs", collections.Counter(("%016x" % x["vs"], "%016x" % x["ps"]) for x in draws).most_common(20))
print("prims", collections.Counter(x["init"] & 0x3F for x in draws).most_common())
for x in draws[:limit]:
    print(" init %08X prim %X src %d idx %d base %08X size %08X mode %d state+%d vs %016x ps %016x" % (
        x["init"], x["init"] & 63, (x["init"] >> 6) & 3, x["init"] >> 16, x["base"], x["size"], x["mode"], x["n"], x["vs"], x["ps"]))
