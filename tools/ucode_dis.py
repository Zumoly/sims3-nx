"""Disassemble Xenos microcode dumped by the capture build. usage: python -I ucode_dis.py <capture_dir>"""
import glob, os, struct, sys

VEC = "add mul max min seq sgt sge sne frc trunc floor mad cndeq cndge cndgt dp4 dp3 dp2add cube max4 setpeq_push setpne_push setpgt_push setpge_push killeq killgt killge killne dst maxa".split()
SCA = {0: "adds", 1: "adds_prev", 2: "muls", 3: "muls_prev", 4: "muls_prev2", 5: "maxs", 6: "mins", 7: "seqs", 8: "sgts", 9: "sges", 10: "snes",
       11: "frcs", 12: "truncs", 13: "floors", 14: "exp", 15: "logc", 16: "log", 17: "rcpc", 18: "rcpf", 19: "rcp", 20: "rsqc", 21: "rsqf", 22: "rsq",
       23: "maxas", 24: "maxasf", 25: "subs", 26: "subs_prev", 27: "setpeq", 28: "setpne", 29: "setpgt", 30: "setpge", 31: "setpinv", 32: "setppop",
       33: "setpclr", 34: "setprstr", 35: "killseq", 36: "killsgt", 37: "killsge", 38: "killsne", 39: "killsone", 40: "sqrt", 42: "mulsc0", 43: "mulsc1",
       44: "addsc0", 45: "addsc1", 46: "subsc0", 47: "subsc1", 48: "sin", 49: "cos", 50: "retain_prev"}
VEC_ARGS = {8: 1, 9: 1, 10: 1, 11: 3, 12: 3, 13: 3, 14: 3, 17: 3, 19: 1, 29: 2}
C = "xyzw"


def bits(v, lo, n):
    return (v >> lo) & ((1 << n) - 1)


def sbits(v, lo, n):
    x = bits(v, lo, n)
    return x - (1 << n) if x >> (n - 1) else x


def dst_swiz(sw):
    return "".join("xyzw01?_"[(sw >> (3 * i)) & 7] for i in range(4))


def alu_swiz(sw):
    return "".join(C[((sw >> (2 * i)) + i) & 3] for i in range(4))


def src(i, w0, w1, w2):
    reg = bits(w2, {1: 16, 2: 8, 3: 0}[i], 8)
    sel = bits(w2, {1: 31, 2: 30, 3: 29}[i], 1)
    sw = bits(w1, {1: 16, 2: 8, 3: 0}[i], 8)
    neg = bits(w1, {1: 26, 2: 25, 3: 24}[i], 1)
    s = "-" if neg else ""
    if sel:
        r = "r%d" % (reg & 63)
        if reg & 0x40:
            r += "[aL]"
        if reg & 0x80:
            r = "|%s|" % r
    else:
        r = "c%d" % reg
    return "%s%s.%s" % (s, r, alu_swiz(sw))


def mask(m):
    return "".join(C[i] if m >> i & 1 else "_" for i in range(4))


def alu(w, is_ps):
    w0, w1, w2 = w
    vop, sop = bits(w2, 24, 5), bits(w0, 26, 6)
    vm, sm = bits(w0, 16, 4), bits(w0, 20, 4)
    ex = bits(w0, 15, 1)
    vd, sd = bits(w0, 0, 6), bits(w0, 8, 6)
    out = []
    if ex:
        name = ("oC%d" % vd) if is_ps else ("oPos" if vd == 62 else "oPts" if vd == 63 else "o%d" % vd)
        vm2, sm2 = vm & ~sm & 15, sm & ~vm & 15
        one = vm & sm
    else:
        name = "r%d" % vd
        vm2, sm2, one = vm, sm, 0
    if vm2 or (vop not in (0,) and not ex and not sm):
        args = [1, 2, 3][:VEC_ARGS.get(vop, 2)]
        out.append("%s.%s = %s%s %s" % (name, mask(vm2), VEC[vop] if vop < len(VEC) else vop, "_sat" if bits(w0, 24, 1) else "",
                                         ", ".join(src(a, w0, w1, w2) for a in args)))
    if sm2:
        dn = name if ex else "r%d" % sd
        out.append("%s.%s = %s%s %s" % (dn, mask(sm2), SCA.get(sop, sop), "_sat" if bits(w0, 25, 1) else "", src(3, w0, w1, w2)))
    if one:
        out.append("%s.%s = 1" % (name, mask(one)))
    if ex and bits(w0, 14, 1):
        z = 15 & ~(vm | sm)
        if z:
            out.append("%s.%s = 0" % (name, mask(z)))
    return out or ["nop"]


def fetch(w):
    w0, w1, w2 = w
    op = bits(w0, 0, 5)
    dst = bits(w0, 12, 6)
    if op == 0:
        idx = bits(w0, 20, 5) * 3 + bits(w0, 25, 2)
        mini = bits(w1, 30, 1)
        return "vfetch%s r%d.%s = r%d, vf%d fmt=%d stride=%d offset=%d signed=%d norm=%d" % (
            "_mini" if mini else "", dst, dst_swiz(bits(w1, 0, 12)), bits(w0, 5, 6), idx, bits(w1, 16, 6), bits(w2, 0, 8), sbits(w2, 8, 23),
            bits(w1, 12, 1), 0 if bits(w1, 13, 1) else 1)
    if op == 1:
        return "tfetch r%d.%s = r%d, tf%d dim=%d denorm=%d swz=%02x" % (dst, dst_swiz(bits(w1, 0, 12)), bits(w0, 5, 6), bits(w0, 20, 5), bits(w2, 14, 2),
                                                                       bits(w0, 25, 1), bits(w0, 26, 6))
    return "fetch_op%d r%d" % (op, dst)


def disasm(path):
    data = open(path, "rb").read()
    d = struct.unpack(">%dI" % (len(data) // 4), data)
    is_ps = os.path.basename(path).startswith("ps_")
    print("=====", os.path.basename(path), len(d), "dwords")
    i, done = 0, False
    while i + 2 < len(d) and not done:
        a0, a1, a2 = d[i:i + 3]
        pair = [(a0, a1 & 0xFFFF), ((a1 >> 16) | (a2 << 16) & 0xFFFFFFFF, a2 >> 16)]
        for lo, hi in pair:
            op = bits(hi, 12, 4)
            if op == 12:
                print("  alloc type=%d size=%d" % (bits(hi, 9, 2), bits(lo, 0, 3)))
            elif op in (1, 2, 3, 4, 5, 6, 13, 14):
                addr, count, seq = bits(lo, 0, 12), bits(lo, 12, 3), bits(lo, 16, 12)
                print("  exec%s addr=%d count=%d seq=%03x" % ("_end" if op in (2, 4, 6, 14) else "", addr, count, seq))
                for n in range(count):
                    p = (addr + n) * 3
                    ins = d[p:p + 3]
                    if len(ins) < 3:
                        print("    <truncated>")
                        continue
                    if seq >> (2 * n) & 1:
                        print("    ", fetch(ins))
                    else:
                        for line in alu(ins, is_ps):
                            print("    ", line)
                if op in (2, 4, 6, 14):
                    done = True
                    break
            elif op:
                print("  cf op %d" % op)
        i += 3


for p in sorted(glob.glob(os.path.join(sys.argv[1], "*s_*.bin"))):
    disasm(p)
