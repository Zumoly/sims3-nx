"""Prints guest memory saved in a frame capture. usage: python capmem.py <frame.bin> <hex address> [bytes]"""
import struct, sys

d = open(sys.argv[1], "rb").read()
want = int(sys.argv[2], 16)
count = int(sys.argv[3]) if len(sys.argv) > 3 else 64
p = 8
while p < len(d):
    tag = struct.unpack_from("<I", d, p)[0]
    p += 4
    if tag == 0x53474552:
        c = struct.unpack_from("<I", d, p)[0]
        p += 4 + 4 * c
    elif tag == 0x57415244:
        p += 16 + 16
        c = struct.unpack_from("<I", d, p)[0]
        p += 4 + c * 8
    elif tag == 0x524D454D:
        kind, addr, size = struct.unpack_from("<III", d, p)
        p += 12
        if addr <= want < addr + size:
            data = d[p + want - addr:p + min(size, want - addr + count)]
            print("range %08X size %X kind %d" % (addr, size, kind))
            for i in range(0, len(data), 16):
                print("%08X  %s" % (want + i, " ".join("%02X" % b for b in data[i:i + 16])))
            sys.exit(0)
        p += (size + 3) & ~3
    elif tag == 0x50415753:
        p += 12
    else:
        break
print("not captured")
