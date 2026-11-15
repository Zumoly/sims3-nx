"""Replay a captured frame in software, to prove the draw logic before writing it for deko3d.
usage: python capsim.py <frame.bin> <out.png> [max_draws]"""
import os, struct, sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "work", "pydeps"))
import numpy as np
from PIL import Image

path, out_png = sys.argv[1], sys.argv[2]
max_draws = int(sys.argv[3]) if len(sys.argv) > 3 else 1 << 30
d = open(path, "rb").read()
p = 8


def u32():
    global p
    v = struct.unpack_from("<I", d, p)[0]
    p += 4
    return v


regs = None
events, ranges = [], []
while p < len(d):
    tag = u32()
    if tag == 0x53474552:
        n = u32()
        regs = list(struct.unpack_from("<%dI" % n, d, p))
        p += 4 * n
    elif tag == 0x57415244:
        init, pred, base, size = u32(), u32(), u32(), u32()
        vs, ps = struct.unpack_from("<QQ", d, p)
        p += 16
        n = u32()
        delta = struct.unpack_from("<%dI" % (n * 2), d, p)
        p += n * 8
        events.append((init, base, size, "%016x" % vs, "%016x" % ps, delta))
    elif tag == 0x524D454D:
        kind, addr, size = u32(), u32(), u32()
        ranges.append((addr, size, p))
        p += (size + 3) & ~3
    elif tag == 0x50415753:
        p += 12
    else:
        raise SystemExit("bad tag at %d" % p)


def mem(addr, size):
    addr &= 0x1FFFFFFF
    for a, s, o in ranges:
        if a <= addr and addr + size <= a + s:
            return d[o + addr - a:o + addr - a + size]
    best = None
    for a, s, o in ranges:
        if a <= addr < a + s and (best is None or s > best[1]):
            best = (a, s, o)
    if best:
        a, s, o = best
        return d[o + addr - a:o + s] + b"\0" * (addr + size - a - s)
    return None


def swap(buf, endian):
    arr = np.frombuffer(buf[:len(buf) & ~3], dtype=np.uint8).copy()
    if endian == 1:
        arr = arr.reshape(-1, 2)[:, ::-1].ravel()
    elif endian == 2:
        arr = arr.reshape(-1, 4)[:, ::-1].ravel()
    elif endian == 3:
        arr = arr.reshape(-1, 2, 2)[:, ::-1, :].ravel()
    return arr


def tiled(x, y, pitch, log2):
    pitch = (pitch + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (log2 + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << log2
    off = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return ((off & ~0x1FF) << 3) + ((y & 16) << 7) + ((off & 0x1C0) << 2) + ((((y & 8) >> 2) + (x >> 3) & 3) << 6) + (off & 0x3F)


def blocks_565(c):
    r = ((c >> 11) & 31) * 255 / 31
    g = ((c >> 5) & 63) * 255 / 63
    b = (c & 31) * 255 / 31
    return np.stack([r, g, b], -1)


def decode_colour(blk, punch):
    c0 = blk[:, 0].astype(np.int64) | (blk[:, 1].astype(np.int64) << 8)
    c1 = blk[:, 2].astype(np.int64) | (blk[:, 3].astype(np.int64) << 8)
    idx = blk[:, 4].astype(np.int64) | blk[:, 5].astype(np.int64) << 8 | blk[:, 6].astype(np.int64) << 16 | blk[:, 7].astype(np.int64) << 24
    a, b = blocks_565(c0), blocks_565(c1)
    four = (c0 > c1) | (not punch)
    p2 = np.where(four[:, None], (2 * a + b) / 3, (a + b) / 2)
    p3 = np.where(four[:, None], (a + 2 * b) / 3, 0)
    pal = np.stack([a, b, p2, p3], 1)
    sel = np.stack([(idx >> (2 * i)) & 3 for i in range(16)], 1)
    rgb = np.take_along_axis(pal, sel[:, :, None].repeat(3, 2), 1)
    alpha = np.where(punch & (~four[:, None]) & (sel == 3), 0, 255) if punch else np.full(sel.shape, 255)
    return rgb, alpha


def decode_alpha5(blk):
    a0 = blk[:, 0].astype(np.float32)
    a1 = blk[:, 1].astype(np.float32)
    bits = np.zeros(len(blk), dtype=np.uint64)
    for i in range(6):
        bits |= blk[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
    sel = np.stack([((bits >> np.uint64(3 * i)) & np.uint64(7)).astype(np.int64) for i in range(16)], 1)
    big = a0 > a1
    pal = [a0, a1]
    for i in range(1, 7):
        pal.append(np.where(big, ((7 - i) * a0 + i * a1) / 7, ((5 - i) * a0 + i * a1) / 5 if i < 5 else (0 if i == 5 else 255)))
    return np.take_along_axis(np.stack(pal, 1), sel, 1)


texture_cache = {}


def texture(first):
    w0, w1, w2, w3 = [regs[first + i] for i in range(4)]
    key = (w0, w1, w2, w3)
    if key in texture_cache:
        return texture_cache[key]
    fmt = w1 & 63
    endian = (w1 >> 6) & 3
    base = (w1 >> 12) << 12
    width, height = (w2 & 0x1FFF) + 1, ((w2 >> 13) & 0x1FFF) + 1
    pitch = ((w0 >> 22) & 0x1FF) << 5 or (width + 31) & ~31
    is_tiled = w0 >> 31
    compressed = fmt in (18, 19, 20)
    bw = 4 if compressed else 1
    log2 = {18: 3, 19: 4, 20: 4, 2: 0, 6: 2}.get(fmt, 2)
    bytes_per = 1 << log2
    bwidth, bheight = (width + bw - 1) // bw, (height + bw - 1) // bw
    bpitch = pitch // bw
    rows = (bheight + 31) & ~31
    raw = mem(base, bpitch * rows * bytes_per)
    if raw is None:
        texture_cache[key] = None
        return None
    data = swap(raw, endian)
    yy, xx = np.meshgrid(np.arange(bheight, dtype=np.int64), np.arange(bwidth, dtype=np.int64), indexing="ij")
    off = tiled(xx, yy, bpitch, log2) if is_tiled else (yy * bpitch + xx) * bytes_per
    off = off.ravel()
    good = off + bytes_per <= len(data)
    off = np.where(good, off, 0)
    blocks = data[off[:, None] + np.arange(bytes_per)]
    if compressed:
        if fmt == 18:
            rgb, a = decode_colour(blocks, True)
        else:
            rgb, _ = decode_colour(blocks[:, 8:], False)
            a = decode_alpha5(blocks) if fmt == 20 else (np.stack([(blocks[:, i // 2] >> (4 * (i & 1))) & 15 for i in range(16)], 1) * 17)
        tex = np.concatenate([rgb, a[:, :, None]], 2).reshape(bheight, bwidth, 4, 4, 4 // 1 if False else 1).squeeze(-1) if False else None
        c = np.concatenate([rgb, a[:, :, None].astype(rgb.dtype)], 2).reshape(bheight, bwidth, 4, 4, 4)
        tex = c.transpose(0, 2, 1, 3, 4).reshape(bheight * 4, bwidth * 4, 4)[:height, :width]
    elif fmt == 2:
        v = blocks[:, 0].reshape(bheight, bwidth)
        tex = np.stack([v, v, v, v], -1)
    else:
        tex = blocks.reshape(bheight, bwidth, 4)[:, :, [3, 2, 1, 0]]  # low byte is X after the endian swap
    tex = tex.astype(np.float32) / 255.0
    sw = (w3 >> 1) & 0xFFF
    sel = [(sw >> (3 * i)) & 7 for i in range(4)]
    chans = []
    for s in sel:
        if s < 4:
            chans.append(tex[:, :, s])
        else:
            chans.append(np.full(tex.shape[:2], 0.0 if s == 4 else 1.0, np.float32))
    tex = np.stack(chans, -1)
    print("  texture %08X %dx%d fmt %d tiled %d endian %d swizzle %s" % (base, width, height, fmt, is_tiled, endian, sel))
    texture_cache[key] = tex
    return tex


def sample(tex, u, v):
    if tex is None:
        return np.ones(u.shape + (4,), np.float32)
    h, w = tex.shape[:2]
    x = np.clip(u, 0, 1) * w - 0.5
    y = np.clip(v, 0, 1) * h - 0.5
    x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
    fx, fy = (x - x0)[..., None], (y - y0)[..., None]
    x0c, x1c = np.clip(x0, 0, w - 1), np.clip(x0 + 1, 0, w - 1)
    y0c, y1c = np.clip(y0, 0, h - 1), np.clip(y0 + 1, 0, h - 1)
    return ((tex[y0c, x0c] * (1 - fx) + tex[y0c, x1c] * fx) * (1 - fy) + (tex[y1c, x0c] * (1 - fx) + tex[y1c, x1c] * fx) * fy)


def fconst(pixel, i):
    first = (0x4400 if pixel else 0x4000) + i * 4
    return np.array(struct.unpack("<4f", struct.pack("<4I", *regs[first:first + 4])), np.float32)


def rgba8(words):
    # Component 0 is the low byte (spirv_translator_fetch.cpp, packed_offsets).
    return np.stack([(words >> (8 * i)) & 255 for i in range(4)], 1).astype(np.float32) / 255.0


def vertices(vs, count, indices):
    """Returns positions Nx4 and interpolators {slot: Nx4}."""
    slot = 0 if vs in ("539ea9e08aa83f0c", "a4ae7c2b7c1818c4") else 95
    w0, w1 = regs[0x4800 + slot * 2], regs[0x4800 + slot * 2 + 1]
    addr, endian, size = w0 & 0x1FFFFFFC, w1 & 3, ((w1 >> 2) & 0xFFFFFF) * 4
    raw = mem(addr, size)
    if raw is None:
        return None
    data = swap(raw, endian)
    words = np.frombuffer(data.tobytes(), dtype="<u4")
    floats = np.frombuffer(data.tobytes(), dtype="<f4")
    stride = {"92e80366cb1c386b": 3, "6dbe72b7b49efab7": 7, "ae95319b2aa28a09": 7, "7f64754ac3dde353": 2, "5db89eb8c07cc32f": 1,
              "539ea9e08aa83f0c": 7, "a4ae7c2b7c1818c4": 2, "cfc2fa687333e466": 6}.get(vs)
    if stride is None:
        return None
    idx = indices if indices is not None else np.arange(count)
    idx = idx[idx * stride + stride <= len(words)]
    base = idx * stride
    out = {}
    if vs in ("92e80366cb1c386b", "7f64754ac3dde353", "5db89eb8c07cc32f"):
        w = words[base].astype(np.int64)
        x = ((w >> 16) & 0xFFFF).astype(np.int32)
        y = (w & 0xFFFF).astype(np.int32)
        x = np.where(x >= 32768, x - 65536, x).astype(np.float32)
        y = np.where(y >= 32768, y - 65536, y).astype(np.float32)
        # Component 0 is the low half and the fetch swizzle is yx01, so r.x is the high half.
        r = np.stack([x, y, np.zeros_like(x), np.ones_like(x)], 1)
        px = r @ fconst(False, 0)
        py = r @ fconst(False, 1)
        pos = np.stack([px, py, np.zeros_like(px), np.ones_like(px)], 1)
        if vs == "92e80366cb1c386b":
            out[1] = rgba8(words[base + 1])
            out[2] = rgba8(words[base + 2])
            out[0] = np.stack([r @ fconst(False, 4), r @ fconst(False, 5), np.zeros_like(x), np.ones_like(x)], 1)
        elif vs == "7f64754ac3dde353":
            out[0] = rgba8(words[base + 1])
    elif vs in ("6dbe72b7b49efab7", "ae95319b2aa28a09"):
        r = np.stack([floats[base + i] for i in range(4)], 1)
        px, py = r @ fconst(False, 0), r @ fconst(False, 1)
        pos = np.stack([px, py, r[:, 2], r[:, 3]], 1)
        uv = np.stack([floats[base + 4], floats[base + 5], np.zeros(len(base)), np.ones(len(base))], 1)
        out[0] = uv
        if vs == "ae95319b2aa28a09":
            out[1] = rgba8(words[base + 6])[:, [2, 1, 0, 3]]
    elif vs == "cfc2fa687333e466":
        pos = np.stack([floats[base + i] for i in range(4)], 1)
        out[0] = np.stack([floats[base + 4], floats[base + 5], np.zeros(len(base)), np.ones(len(base))], 1)
    elif vs == "539ea9e08aa83f0c":
        pos = np.stack([floats[base], floats[base + 1], floats[base + 2], np.ones(len(base))], 1)
        out[0] = np.stack([floats[base + 3 + i] for i in range(4)], 1)
    else:
        pos = np.stack([floats[base], floats[base + 1], np.ones(len(base)), np.ones(len(base))], 1)
    return pos, out


def shade(ps, attrs, u_tex):
    c2, c3 = fconst(True, 2), fconst(True, 3)
    if ps == "438c2af84c78a133":
        return attrs[0]
    if ps == "03119cfd11beb173":
        return np.broadcast_to(fconst(True, 0), attrs[0].shape if 0 in attrs else (1, 4))
    if ps == "7a27a638de062231":
        return attrs[0] * c2 + c3
    t = sample(u_tex, attrs[0][..., 0], attrs[0][..., 1])
    if ps == "a15c6c9c2d249375":
        return t
    if ps == "1d03653c7a4cff4b":
        return t * c2 + c3
    if ps == "43f1e01c2eb935be":
        r1, r2 = attrs[1], attrs[2]
        m = (t - r1) * r2[..., 2:3] + r1
        rgb = m[..., :3] * c2[:3] + c3[:3]
        a = (m[..., 3] * c2[3] + c3[3]) * r2[..., 3]
        return np.concatenate([rgb, a[..., None]], -1)
    if ps == "4d80b8b337ff38f4":
        r1 = attrs[1]
        a = (r1[..., 3] * c2[3] + c3[3]) * t[..., 3]
        return np.concatenate([r1[..., :3] * c2[:3] + c3[:3], a[..., None]], -1)
    return t


def factor(f, src, dst):
    if f == 0:
        return np.zeros_like(src)
    if f == 1:
        return np.ones_like(src)
    a, b = src[..., 3:4], dst[..., 3:4]
    return {4: src, 5: 1 - src, 6: a, 7: 1 - a, 8: dst, 9: 1 - dst, 10: b, 11: 1 - b}.get(f, np.ones_like(src))


W, H = 1280, 720
fb = np.zeros((H, W, 4), np.float32)
stencil = np.zeros((H, W), np.uint8)
stats = {}
for n, (init, base, size, vs, ps, delta) in enumerate(events):
    for i in range(0, len(delta), 2):
        regs[delta[i]] = delta[i + 1]
    if n >= max_draws:
        break
    mode, prim, source, count = regs[0x2208] & 7, init & 63, (init >> 6) & 3, init >> 16
    pitch = regs[0x2000] & 0x3FFF
    if mode != 4 or pitch != 1280 or vs in ("8bb7e189d92e3def",):
        stats["skipped"] = stats.get("skipped", 0) + 1
        continue
    indices = None
    if source == 0:
        raw = mem(base, count * 2)
        if raw is None:
            continue
        indices = np.frombuffer(swap(raw, (size >> 30) & 3).tobytes(), dtype="<u2").astype(np.int64)[:count]
    v = vertices(vs, count, indices)
    if v is None:
        stats["no_vertices"] = stats.get("no_vertices", 0) + 1
        continue
    pos, attrs = v
    vte = regs[0x2206]
    sx, ox, sy, oy = [struct.unpack("<f", struct.pack("<I", regs[0x210F + i]))[0] for i in range(4)]
    x = pos[:, 0] / pos[:, 3]
    y = pos[:, 1] / pos[:, 3]
    if vte & 1:
        x = x * sx
    if vte & 2:
        x = x + ox
    if vte & 4:
        y = y * sy
    if vte & 8:
        y = y + oy
    tris = []
    if prim == 8:
        for t in range(0, len(x) - 2, 3):
            tris += [(t, t + 1, t + 2), (t + 2, t + 1, -t - 1)]
    else:
        for t in range(0, len(x) - 2, 3):
            tris.append((t, t + 1, t + 2))
    tex = texture(0x4800) if ps not in ("438c2af84c78a133", "03119cfd11beb173", "7a27a638de062231") else None
    bc = regs[0x2201]
    src_c, op_c, dst_c = bc & 31, (bc >> 5) & 7, (bc >> 8) & 31
    tl, br = regs[0x2081], regs[0x2082]
    cx0, cy0 = tl & 0x7FFF, (tl >> 16) & 0x7FFF
    cx1, cy1 = min(br & 0x7FFF, W), min((br >> 16) & 0x7FFF, H)
    for a, b, c in tris:
        if c < 0:  # fourth corner of a rectangle
            t = -c - 1
            q = [(x[a], y[a]), (x[b], y[b]), (x[t] + x[a + 0] * 0 + (x[a] + x[b] - 2 * x[t]) * 1, 0)]
            p0, p1, p2 = t, t + 1, t + 2
            px3, py3 = x[p0] + x[p2] - x[p1], y[p0] + y[p2] - y[p1]
            tx = np.array([x[p2], x[p0], px3])
            ty = np.array([y[p2], y[p0], py3])
            ta = {k: np.stack([val[p2], val[p0], val[p0] + val[p2] - val[p1]]) for k, val in attrs.items()}
        else:
            tx, ty = x[[a, b, c]], y[[a, b, c]]
            ta = {k: val[[a, b, c]] for k, val in attrs.items()}
        if not (np.isfinite(tx).all() and np.isfinite(ty).all()):
            continue
        minx, maxx = max(int(np.floor(tx.min())), cx0, 0), min(int(np.ceil(tx.max())) + 1, cx1, W)
        miny, maxy = max(int(np.floor(ty.min())), cy0, 0), min(int(np.ceil(ty.max())) + 1, cy1, H)
        if minx >= maxx or miny >= maxy:
            continue
        gy, gx = np.mgrid[miny:maxy, minx:maxx]
        gx = gx + 0.5
        gy = gy + 0.5
        denom = (ty[1] - ty[2]) * (tx[0] - tx[2]) + (tx[2] - tx[1]) * (ty[0] - ty[2])
        if abs(denom) < 1e-9:
            continue
        w0 = ((ty[1] - ty[2]) * (gx - tx[2]) + (tx[2] - tx[1]) * (gy - ty[2])) / denom
        w1 = ((ty[2] - ty[0]) * (gx - tx[2]) + (tx[0] - tx[2]) * (gy - ty[2])) / denom
        w2 = 1 - w0 - w1
        inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
        if not inside.any():
            continue
        interp = {k: w0[..., None] * val[0] + w1[..., None] * val[1] + w2[..., None] * val[2] for k, val in ta.items()}
        if not interp:
            interp = {0: np.zeros(gx.shape + (4,), np.float32)}
        dc = regs[0x2200]
        passed = inside
        if dc & 1:  # stencil: the reference is on the left of the comparison, as on Xenos
            word = regs[0x210D]
            ref, read_mask, write_mask = word & 255, (word >> 8) & 255, (word >> 16) & 255
            func, zpass = (dc >> 8) & 7, (dc >> 14) & 7
            s = stencil[miny:maxy, minx:maxx]
            a, b = ref & read_mask, s & read_mask
            test = [np.zeros_like(s, bool), a < b, a == b, a <= b, a > b, a != b, a >= b, np.ones_like(s, bool)][func]
            passed = inside & test
            new = {0: s, 1: s * 0, 2: s * 0 + ref, 3: np.minimum(s.astype(int) + 1, 255), 4: np.maximum(s.astype(int) - 1, 0),
                   5: ~s, 6: (s.astype(int) + 1) & 255, 7: (s.astype(int) - 1) & 255}[zpass]
            new = (s & (255 ^ write_mask)) | (np.asarray(new).astype(np.uint8) & write_mask)
            s[passed] = new[passed]
        if not (regs[0x2104] & 0xF):
            continue
        colour = np.clip(shade(ps, interp, tex), 0, 1).astype(np.float32)
        dst = fb[miny:maxy, minx:maxx]
        res = colour * factor(src_c, colour, dst) + dst * factor(dst_c, colour, dst)
        dst[passed] = np.clip(res, 0, 1)[passed]
    stats["drawn"] = stats.get("drawn", 0) + 1
print("stats", stats)
Image.fromarray((fb[:, :, :3] * 255 + 0.5).astype(np.uint8)).save(out_png)
print("wrote", out_png)
