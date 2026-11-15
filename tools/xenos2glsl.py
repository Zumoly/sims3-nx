"""Translate Xenos microcode to GLSL for uam. Run offline over the shaders the game has used.
usage: python -I xenos2glsl.py <capture_dir> <out_dir>

Field layouts and operation semantics follow sdk/include/rex/graphics/format/ucode.h,
sdk/src/graphics/pipeline/shader/translator.cpp and interpreter.cpp.

Runtime contract, also written to manifest.json for each shader:
  uniform block 0: 256 float constants of that stage.
  uniform block 1: boolean constants, the viewport transform and the parameter register.
  vertex stage: a storage buffer per vertex fetch constant in manifest order, words already in host
    byte order, read with gl_VertexID. Vertex formats are decoded here rather than by vertex attributes.
  textures: binding = fetch constant index.
Registers are plain variables unless a shader addresses them through aL."""
import glob, json, os, struct, sys


def bits(v, lo, n):
    return (v >> lo) & ((1 << n) - 1)


def sbits(v, lo, n):
    x = bits(v, lo, n)
    return x - (1 << n) if x >> (n - 1) else x


C = "xyzw"
EXEC_OPS = {1, 2, 3, 4, 5, 6, 13, 14}
END_OPS = {2, 4, 6, 14}
BOOL_OPS = {3, 4, 13, 14}
PRED_OPS = {5, 6}


class Unsupported(Exception):
    pass


class Alu:
    def __init__(self, w):
        w0, w1, w2 = w
        self.vd, self.vd_rel, self.abs_const = bits(w0, 0, 6), bits(w0, 6, 1), bits(w0, 7, 1)
        self.sd, self.sd_rel, self.export = bits(w0, 8, 6), bits(w0, 14, 1), bits(w0, 15, 1)
        self.vm, self.sm = bits(w0, 16, 4), bits(w0, 20, 4)
        self.vclamp, self.sclamp, self.sop = bits(w0, 24, 1), bits(w0, 25, 1), bits(w0, 26, 6)
        self.swiz = {3: bits(w1, 0, 8), 2: bits(w1, 8, 8), 1: bits(w1, 16, 8)}
        self.neg = {3: bits(w1, 24, 1), 2: bits(w1, 25, 1), 1: bits(w1, 26, 1)}
        self.predicated, self.pred_condition = bits(w1, 28, 1), bits(w1, 27, 1)
        self.const_a0, self.const1_rel, self.const0_rel = bits(w1, 29, 1), bits(w1, 30, 1), bits(w1, 31, 1)
        self.reg = {3: bits(w2, 0, 8), 2: bits(w2, 8, 8), 1: bits(w2, 16, 8)}
        self.vop = bits(w2, 24, 5)
        self.sel = {3: bits(w2, 29, 1), 2: bits(w2, 30, 1), 1: bits(w2, 31, 1)}

    def const_addressed(self, i):
        if i == 1:
            return self.const0_rel
        if i == 2:
            return self.const0_rel if self.sel[1] else self.const1_rel
        return self.const0_rel if (self.sel[1] and self.sel[2]) else self.const1_rel


def component(swizzle, i):
    return ((swizzle >> (2 * i)) + i) & 3


class Translator:
    def __init__(self, data, is_ps, name):
        self.d = struct.unpack(">%dI" % (len(data) // 4), data)
        self.is_ps, self.name = is_ps, name
        self.fetch_slots, self.textures = [], {}
        self.max_reg, self.exports, self.colours = 0, set(), set()
        self.uses_al = False
        self.body = []
        self.mini = None
        self.written, self.inputs = {}, set()
        self.helpers = set()
        self.depth = False
        self.has_kill = False

    # registers ---------------------------------------------------------------------------------

    def note_read(self, n):
        self.max_reg = max(self.max_reg, n)
        if self.written.get(n, 0) != 15:
            self.inputs.add(n)

    def note_write(self, n, mask=15):
        self.max_reg = max(self.max_reg, n)
        self.written[n] = self.written.get(n, 0) | mask

    def reg(self, n, relative):
        if relative:
            self.uses_al = True
            raise Unsupported("aL relative register")
        return "r%d" % n

    def source(self, a, i, count=4):
        s = a.reg[i]
        if a.sel[i]:
            n = s & 63
            self.note_read(n)
            text = self.reg(n, s & 0x40)
            is_abs = s & 0x80
        else:
            if a.const_addressed(i):
                if not a.const_a0:
                    raise Unsupported("aL addressed constant")
                text = "c[(a0 + %d) & 255]" % s
            else:
                text = "c[%d]" % s
            is_abs = a.abs_const
        sw = a.swiz[i]
        if count == 4:
            text += "." + "".join(C[component(sw, n)] for n in range(4))
        elif count == 1:
            text += "." + C[component(sw, 3)]
        else:
            text += "." + C[component(sw, 3)] + C[component(sw, 0)]
        if is_abs:
            text = "abs(%s)" % text
        return "(-%s)" % text if a.neg[i] else text

    # ALU ---------------------------------------------------------------------------------------

    def vector(self, a):
        v = a.vop
        one = lambda: self.source(a, 1)
        two = lambda: (self.source(a, 1), self.source(a, 2))
        three = lambda: (self.source(a, 1), self.source(a, 2), self.source(a, 3))
        if v == 0:
            return "%s + %s" % two()
        if v == 1:
            return "%s * %s" % two()
        if v == 2:
            x, y = two()
            return "max(%s, %s)" % (x, y)
        if v == 3:
            return "min(%s, %s)" % two()
        if v in (4, 5, 6, 7):
            fn = {4: "equal", 5: "greaterThan", 6: "greaterThanEqual", 7: "notEqual"}[v]
            return "vec4(%s(%s, %s))" % ((fn,) + two())
        if v == 8:
            return "fract(%s)" % one()
        if v == 9:
            return "trunc(%s)" % one()
        if v == 10:
            return "floor(%s)" % one()
        if v == 11:
            return "%s * %s + %s" % three()
        if v in (12, 13, 14):
            fn = {12: "equal", 13: "greaterThanEqual", 14: "greaterThan"}[v]
            x, y, z = three()
            return "mix(%s, %s, %s(%s, vec4(0.0)))" % (z, y, fn, x)
        if v == 15:
            return "vec4(dot(%s, %s))" % two()
        if v == 16:
            x, y = two()
            return "vec4(dot((%s).xyz, (%s).xyz))" % (x, y)
        if v == 17:
            x, y, z = three()
            return "vec4((%s).x * (%s).x + (%s).y * (%s).y + (%s).x)" % (x, y, x, y, z)
        if v == 18:
            self.helpers.add("cube")
            return "x_cube(%s)" % self.source(a, 1)
        if v == 19:
            self.helpers.add("max4")
            return "x_max4(%s)" % one()
        if v in (24, 25, 26, 27):
            if not self.is_ps:
                raise Unsupported("vertex kill")
            self.has_kill = True
            fn = {24: "equal", 25: "greaterThan", 26: "greaterThanEqual", 27: "notEqual"}[v]
            x, y = two()
            self.emit("if (any(%s(%s, %s))) discard;" % (fn, x, y))
            return "vec4(0.0)"
        if v == 29:
            x, y = two()
            self.emit("a0 = clamp(int(floor((%s).w + 0.5)), -256, 255);" % x)
            return "max(%s, %s)" % (x, y)
        raise Unsupported("vector op %d" % v)

    def scalar(self, a):
        s = a.sop
        one = lambda: self.source(a, 3, 1)
        pair = lambda: self.source(a, 3, 2)
        if s in (0, 2, 5, 6, 23, 24, 25):
            self.emit("sp = %s;" % pair())
            x, y = "sp.x", "sp.y"
            if s == 23:
                self.emit("a0 = clamp(int(floor(sp.x + 0.5)), -256, 255);")
            elif s == 24:
                self.emit("a0 = clamp(int(floor(sp.x)), -256, 255);")
            return {0: "%s + %s" % (x, y), 2: "%s * %s" % (x, y), 5: "max(%s, %s)" % (x, y), 6: "min(%s, %s)" % (x, y),
                    23: "max(%s, %s)" % (x, y), 24: "max(%s, %s)" % (x, y), 25: "%s - %s" % (x, y)}[s]
        if s in (27, 28, 29, 30):
            test = {27: "%s == 0.0", 28: "%s != 0.0", 29: "%s > 0.0", 30: "%s >= 0.0"}[s] % one()
            self.emit("p0 = %s;" % test)
            return "p0 ? 0.0 : 1.0"
        if s in (42, 43, 44, 45, 46, 47):
            comp = C[component(a.swiz[3], 3)]
            other = C[component(a.swiz[3], 0)]
            if a.const_addressed(3):
                if not a.const_a0:
                    raise Unsupported("aL addressed scalar constant")
                k = "c[(a0 + %d) & 255].%s" % (a.reg[3], comp)
            else:
                k = "c[%d].%s" % (a.reg[3], comp)
            t = (s & 1) | (a.sel[3] << 1) | (a.swiz[3] & 0x3C)
            self.note_read(t)
            r = "r%d.%s" % (t, other)
            if a.abs_const:
                k, r = "abs(%s)" % k, "abs(%s)" % r
            if a.neg[3]:
                k, r = "(-%s)" % k, "(-%s)" % r
            return {42: "%s * %s", 43: "%s * %s", 44: "%s + %s", 45: "%s + %s", 46: "%s - %s", 47: "%s - %s"}[s] % (k, r)
        table = {1: "%s + ps", 3: "%s * ps", 7: "float(%s == 0.0)", 8: "float(%s > 0.0)", 9: "float(%s >= 0.0)",
                 10: "float(%s != 0.0)", 11: "fract(%s)", 12: "trunc(%s)", 13: "floor(%s)", 14: "x_fin(pow(2.0, %s))",
                 16: "x_fin(log2(%s))", 19: "x_fin(1.0 / %s)", 22: "x_fin(inversesqrt(%s))", 26: "%s - ps", 40: "sqrt(%s)", 48: "sin(%s)",
                 49: "cos(%s)"}
        if s in table:
            return table[s] % one()
        if s == 50:
            return None
        raise Unsupported("scalar op %d" % s)

    def target(self, a, index, mask):
        if a.export:
            if self.is_ps:
                if index <= 3:
                    self.colours.add(index)
                    return "oC%d" % index
                if index == 61:
                    self.depth = True
                    return "oDepth"
                raise Unsupported("pixel export %d" % index)
            if index == 62:
                return "oPos"
            if index == 63:
                return "oPts"
            if index <= 15:
                self.exports.add(index)
                return "o%d" % index
            raise Unsupported("vertex export %d" % index)
        self.note_write(index, mask)
        return "r%d" % index

    def alu(self, ins):
        a = Alu(ins)
        if a.predicated:
            self.emit("if (p0 == %s) {" % ("true" if a.pred_condition else "false"))
        if a.vd_rel or (a.sd_rel and not a.export):
            raise Unsupported("relative destination")
        if a.export:
            vmask, smask = a.vm & ~a.sm & 15, a.sm & ~a.vm & 15
            ones = a.vm & a.sm
            zeros = (15 & ~(a.vm | a.sm)) if a.sd_rel else 0
        else:
            vmask, smask, ones, zeros = a.vm, a.sm, 0, 0
        v = self.vector(a) if (vmask or a.vop in (24, 25, 26, 27)) else None
        s = self.scalar(a) if (smask or a.sop not in (50,)) else None
        if v is not None:
            self.emit("pv = %s;" % (("clamp(%s, 0.0, 1.0)" % v) if a.vclamp else v))
        if s is not None:
            self.emit("ps = %s;" % (("clamp(%s, 0.0, 1.0)" % s) if a.sclamp else s))
        dest = self.target(a, a.vd, vmask | ones | zeros) if (vmask or ones or zeros) else None
        if vmask:
            comps = "".join(C[i] for i in range(4) if vmask >> i & 1)
            self.emit("%s.%s = pv.%s;" % (dest, comps, comps))
        if smask:
            name = self.target(a, a.vd if a.export else a.sd, smask)
            comps = "".join(C[i] for i in range(4) if smask >> i & 1)
            self.emit("%s.%s = %s;" % (name, comps, "ps" if len(comps) == 1 else "vec%d(ps)" % len(comps)))
        for mask, value in ((ones, "1.0"), (zeros, "0.0")):
            for i in range(4):
                if mask >> i & 1:
                    self.emit("%s.%s = %s;" % (dest, C[i], value))
        if a.predicated:
            self.emit("}")

    # fetch -------------------------------------------------------------------------------------

    def apply_swizzle(self, dst, swizzle, value):
        self.note_write(dst, sum(1 << i for i in range(4) if (swizzle >> (3 * i)) & 7 != 7))
        for i in range(4):
            s = (swizzle >> (3 * i)) & 7
            if s == 7:
                continue
            if s == 4:
                self.emit("r%d.%s = 0.0;" % (dst, C[i]))
            elif s == 5:
                self.emit("r%d.%s = 1.0;" % (dst, C[i]))
            else:
                self.emit("r%d.%s = %s.%s;" % (dst, C[i], value, C[s]))

    def vfetch(self, w):
        w0, w1, w2 = w
        src, dst = bits(w0, 5, 6), bits(w0, 12, 6)
        if bits(w0, 11, 1) or bits(w0, 18, 1):
            raise Unsupported("relative vertex fetch")
        predicated = bits(w1, 31, 1)
        if predicated:
            self.emit("if (p0 == %s) {" % ("true" if bits(w2, 31, 1) else "false"))
        comp = bits(w0, 30, 2)
        slot = bits(w0, 20, 5) * 3 + bits(w0, 25, 2)
        fmt, signed = bits(w1, 16, 6), bits(w1, 12, 1)
        norm = 0 if bits(w1, 13, 1) else 1
        stride, offset, exp = bits(w2, 0, 8), sbits(w2, 8, 23), sbits(w1, 24, 6)
        if bits(w1, 30, 1):
            if self.mini is None:
                raise Unsupported("mini fetch first")
            slot, stride = self.mini
        else:
            self.mini = (slot, stride)
        if slot not in self.fetch_slots:
            self.fetch_slots.append(slot)
        binding = self.fetch_slots.index(slot)
        self.note_read(src)
        self.helpers.add("fmt%d" % fmt)
        index = "(int(round(r%d.%s)) * %d + %d)" % (src, C[comp], stride, offset)
        call = "fetch_%d(words_%d, %s, %d, %d)" % (fmt, binding, index, signed, norm)
        if fmt not in FORMATS:
            raise Unsupported("vertex format %d" % fmt)
        scale = (" * %s" % (2.0 ** exp)) if exp else ""
        self.emit("{ vec4 f = %s%s;" % (call, scale))
        self.apply_swizzle(dst, bits(w1, 0, 12), "f")
        self.emit("}")
        if predicated:
            self.emit("}")

    def tfetch(self, w):
        w0, w1, w2 = w
        op, src, dst = bits(w0, 0, 5), bits(w0, 5, 6), bits(w0, 12, 6)
        if bits(w0, 11, 1) or bits(w0, 18, 1):
            raise Unsupported("relative texture fetch")
        predicated = bits(w1, 31, 1)
        if predicated:
            self.emit("if (p0 == %s) {" % ("true" if bits(w2, 31, 1) else "false"))
        index, swizzle = bits(w0, 20, 5), bits(w0, 26, 6)
        dim = bits(w2, 14, 2)
        if op == 19:
            kind = "2d"
        else:
            kind = {1: "2d", 3: "cube"}.get(dim)
        if kind is None:
            raise Unsupported("texture dimension %d" % dim)
        if self.textures.setdefault(index, kind) != kind:
            raise Unsupported("texture %d used as two kinds" % index)
        self.note_read(src)
        coord = "r%d.%s" % (src, "".join(C[(swizzle >> (2 * i)) & 3] for i in range(3)))
        name = "t%d" % index
        denormal = bits(w0, 25, 1)
        offsets = (sbits(w2, 16, 5) * 0.5, sbits(w2, 21, 5) * 0.5)
        if op == 19:
            shift = "vec2(%s, %s)" % offsets
            self.emit("{ vec2 g = (%s).xy * vec2(textureSize(%s, 0)) + %s - 0.5; vec4 f = vec4(fract(g), 0.0, 0.0);" % (
                coord, name, shift))
        elif kind == "cube":
            self.helpers.add("cube_dir")
            self.emit("{ vec4 f = %s(%s, x_cube_dir(%s));" % ("texture" if self.is_ps else "textureLod", name, coord)
                      if self.is_ps else "{ vec4 f = textureLod(%s, x_cube_dir(%s), 0.0);" % (name, coord))
        else:
            lookup = "(%s).xy" % coord
            if denormal:
                lookup += " / vec2(textureSize(%s, 0))" % name
            if offsets != (0.0, 0.0):
                lookup += " + vec2(%s, %s) / vec2(textureSize(%s, 0))" % (offsets[0], offsets[1], name)
            self.emit("{ vec4 f = texture(%s, x_uv(%s));" % (name, lookup) if self.is_ps else
                      "{ vec4 f = textureLod(%s, x_uv(%s), 0.0);" % (name, lookup))
        self.apply_swizzle(dst, bits(w1, 0, 12), "f")
        self.emit("}")
        if predicated:
            self.emit("}")

    def fetch(self, w):
        op = bits(w[0], 0, 5)
        if op == 0:
            self.vfetch(w)
        elif op in (1, 19):
            self.tfetch(w)
        else:
            raise Unsupported("fetch op %d" % op)

    # control flow ------------------------------------------------------------------------------

    def emit(self, text):
        self.body.append("  " + text)

    def run_exec(self, addr, count, seq):
        for n in range(count):
            p = (addr + n) * 3
            ins = self.d[p:p + 3]
            if len(ins) < 3:
                raise Unsupported("truncated exec")
            if seq >> (2 * n) & 1:
                self.fetch(ins)
            else:
                self.alu(ins)

    def control_flow(self):
        # A conditional end only ends the shader if its condition holds, so the walk runs on to the
        # first instruction word or the first unconditional end.
        cfs, i, done, limit = [], 0, False, len(self.d)
        while i + 2 < limit and not done:
            a0, a1, a2 = self.d[i:i + 3]
            for lo, hi in ((a0, a1 & 0xFFFF), (((a1 >> 16) | (a2 << 16)) & 0xFFFFFFFF, a2 >> 16)):
                op = bits(hi, 12, 4)
                cfs.append((lo, hi))
                if op in EXEC_OPS and bits(lo, 12, 3):
                    limit = min(limit, bits(lo, 0, 12) * 3)
                if op == 2:
                    done = True
                    break
            i += 3
        return cfs

    def translate(self):
        cfs = self.control_flow()
        jumps = any(bits(h, 12, 4) == 11 for _, h in cfs)
        if jumps:
            self.emit("int pc = 0;")
            self.emit("while (running) { switch (pc) {")
        for number, (lo, hi) in enumerate(cfs):
            op = bits(hi, 12, 4)
            if jumps:
                self.emit("case %d:" % number)
            if op in EXEC_OPS:
                condition = "true" if bits(hi, 10, 1) else "false"
                conditional = op in BOOL_OPS or op in PRED_OPS
                if op in BOOL_OPS:
                    self.emit("if (x_bool(%d) == %s) {" % (bits(hi, 2, 8), condition))
                elif op in PRED_OPS:
                    self.emit("if (p0 == %s) {" % condition)
                self.run_exec(bits(lo, 0, 12), bits(lo, 12, 3), bits(lo, 16, 12))
                if op in END_OPS:
                    self.emit("running = false; break;")
                if conditional:
                    self.emit("}")
            elif op == 11:
                target = bits(lo, 0, 13)
                wanted = "true" if bits(hi, 10, 1) else "false"
                if bits(lo, 13, 1):
                    test = "true"
                elif bits(lo, 14, 1):
                    test = "p0 == %s" % wanted
                else:
                    test = "x_bool(%d) == %s" % (bits(hi, 2, 8), wanted)
                self.emit("if (%s) { pc = %d; break; }" % (test, target))
            elif op in (0, 12):
                pass
            else:
                raise Unsupported("control flow op %d" % op)
            if jumps and op != 2:
                self.emit("pc = %d; break;" % (number + 1))
        if jumps:
            self.emit("default: running = false; break;")
            self.emit("} }")
        else:
            self.body.insert(0, "  do {")
            self.emit("} while (false);")
        return self.source_text()

    def source_text(self):
        out = ["#version 460", "// generated by tools/xenos2glsl.py from %s" % self.name]
        out.append("layout(std140, binding = 0) uniform Floats { vec4 c[256]; };")
        out.append("layout(std140, binding = 1) uniform State { uvec4 bools[2]; vec4 ndc_scale; vec4 ndc_offset; ivec4 misc; };")
        # On Xenos zero times infinity is zero. Here it is NaN, so results stay finite, and coordinates with them.
        out.append("float x_fin(float v) { return clamp(v, -1e30, 1e30); }")
        out.append("vec2 x_uv(vec2 v) { return clamp(v, vec2(-16384.0), vec2(16384.0)); }")
        for index, kind in sorted(self.textures.items()):
            out.append("layout(binding = %d) uniform %s t%d;" % (index, "samplerCube" if kind == "cube" else "sampler2D", index))
        if self.is_ps:
            inputs = sorted(self.inputs)
            for n in inputs:
                out.append("layout(location = %d) in vec4 v%d;" % (n, n))
            for n in sorted(self.colours):
                out.append("layout(location = %d) out vec4 oC%d;" % (n, n))
        else:
            for i in range(len(self.fetch_slots)):
                out.append("layout(std430, binding = %d) readonly buffer Words%d { uint words_%d[]; };" % (i, i, i))
            for n in sorted(self.exports):
                out.append("layout(location = %d) out vec4 o%d;" % (n, n))
        out.append("bool x_bool(int i) { return ((bools[i >> 7][(i >> 5) & 3] >> (i & 31)) & 1u) != 0u; }")
        if "cube" in self.helpers:
            out.append(CUBE)
        if "cube_dir" in self.helpers:
            out.append(CUBE_DIR)
        if "max4" in self.helpers:
            out.append("vec4 x_max4(vec4 a) { return vec4(max(max(a.x, a.y), max(a.z, a.w))); }")
        if not self.is_ps:
            used = sorted(int(h[3:]) for h in self.helpers if h.startswith("fmt"))
            for i in range(len(self.fetch_slots)):
                for fmt in used:
                    out.append(FORMATS[fmt].replace("WORDS", "words_%d" % i).replace("FETCHNAME", "fetch_%d_%d" % (fmt, i)))
        out.append("void main() {")
        out.append("  vec4 pv = vec4(0.0); float ps = 0.0; vec2 sp = vec2(0.0); int a0 = 0; bool p0 = false;")
        out.append("  bool running = true;")
        for n in range(self.max_reg + 1):
            if self.is_ps and n in self.inputs:
                out.append("  vec4 r%d = v%d;" % (n, n))
            elif not self.is_ps and n == 0:
                out.append("  vec4 r0 = vec4(float(gl_VertexID + misc.w));")  # VGT_INDX_OFFSET
            else:
                out.append("  vec4 r%d = vec4(0.0);" % n)
        if self.is_ps:
            for n in sorted(self.inputs):
                out.append("  if (misc.x == %d) r%d = vec4(gl_FragCoord.xy, 0.0, 0.0);" % (n, n))
            if self.depth:
                out.append("  vec4 oDepth = vec4(0.0);")
            for n in sorted(self.colours):
                out.append("  oC%d = vec4(0.0);" % n)
        else:
            out.append("  vec4 oPos = vec4(0.0, 0.0, 0.0, 1.0); vec4 oPts = vec4(0.0);")
            for n in sorted(self.exports):
                out.append("  o%d = vec4(0.0);" % n)
        text = "\n".join(self.body)
        if not self.is_ps:
            for i in range(len(self.fetch_slots)):
                pass
            for fmt in sorted(int(h[3:]) for h in self.helpers if h.startswith("fmt")):
                for i in range(len(self.fetch_slots)):
                    text = text.replace("fetch_%d(words_%d," % (fmt, i), "fetch_%d_%d(" % (fmt, i))
        out.append(text)
        if self.is_ps:
            if self.depth:
                out.append("  gl_FragDepth = oDepth.x;")
        else:
            # PA_CL_VTE_CNTL: misc.y says W is not reciprocal, misc.z says XY (1) and Z (2) were divided by W.
            out.append("  float w = misc.y != 0 ? oPos.w : 1.0 / oPos.w;")
            out.append("  vec2 xy = (misc.z & 1) != 0 ? oPos.xy * w : oPos.xy;")
            out.append("  float z = (misc.z & 2) != 0 ? oPos.z * w : oPos.z;")
            out.append("  gl_Position = vec4(xy * ndc_scale.xy + ndc_offset.xy * w, z * ndc_scale.z + ndc_offset.z * w, w);")
        out.append("}")
        return "\n".join(line for line in out if line != "") + "\n"


CUBE = """vec4 x_cube(vec4 a) {
  float x = a.z, y = a.w, z = a.x;
  float ax = abs(x), ay = abs(y), az = abs(z);
  if (az >= ax && az >= ay) return vec4(-y, z < 0.0 ? -x : x, 2.0 * z, z < 0.0 ? 5.0 : 4.0);
  if (ay >= ax) return vec4(y < 0.0 ? -z : z, x, 2.0 * y, y < 0.0 ? 3.0 : 2.0);
  return vec4(-y, x < 0.0 ? z : -z, 2.0 * x, x < 0.0 ? 1.0 : 0.0);
}"""

CUBE_DIR = """vec3 x_cube_dir(vec3 p) {
  if (any(isnan(p)) || any(isinf(p))) return vec3(1.0, 0.0, 0.0);
  float s = (p.x - 1.5) * 2.0, t = (p.y - 1.5) * 2.0;
  int face = int(p.z + 0.5);
  if (face == 0) return vec3(1.0, -t, -s);
  if (face == 1) return vec3(-1.0, -t, s);
  if (face == 2) return vec3(s, 1.0, t);
  if (face == 3) return vec3(s, -1.0, -t);
  if (face == 4) return vec3(s, -t, 1.0);
  return vec3(-s, -t, -1.0);
}"""

_NORM = """  if (norm != 0) {
    if (sign != 0) return max(vec4(%s) / %s, vec4(-1.0));
    return vec4(%s) / %s;
  }
  return vec4(%s);
}"""

_UNPACK8 = """vec4 FETCHNAME(int at, int sign, int norm) {
  uint w = WORDS[at];
  ivec4 s = ivec4(bitfieldExtract(int(w), 0, 8), bitfieldExtract(int(w), 8, 8), bitfieldExtract(int(w), 16, 8), bitfieldExtract(int(w), 24, 8));
  vec4 u = vec4(w & 255u, (w >> 8) & 255u, (w >> 16) & 255u, w >> 24);
  if (norm != 0) return sign != 0 ? max(vec4(s) / 127.0, vec4(-1.0)) : u / 255.0;
  return sign != 0 ? vec4(s) : u;
}"""

_UNPACK16 = """vec4 FETCHNAME(int at, int sign, int norm) {
  uint w = WORDS[at];
  vec2 s = vec2(bitfieldExtract(int(w), 0, 16), bitfieldExtract(int(w), 16, 16));
  vec2 u = vec2(w & 65535u, w >> 16);
  if (norm != 0) return vec4(sign != 0 ? max(s / 32767.0, vec2(-1.0)) : u / 65535.0, 0.0, 1.0);
  return vec4(sign != 0 ? s : u, 0.0, 1.0);
}"""

_UNPACK16_4 = """vec4 FETCHNAME(int at, int sign, int norm) {
  uint a = WORDS[at], b = WORDS[at + 1];
  vec4 s = vec4(bitfieldExtract(int(a), 0, 16), bitfieldExtract(int(a), 16, 16), bitfieldExtract(int(b), 0, 16), bitfieldExtract(int(b), 16, 16));
  vec4 u = vec4(a & 65535u, a >> 16, b & 65535u, b >> 16);
  if (norm != 0) return sign != 0 ? max(s / 32767.0, vec4(-1.0)) : u / 65535.0;
  return sign != 0 ? s : u;
}"""

_FLOATS = """vec4 FETCHNAME(int at, int sign, int norm) {
  return vec4(%s);
}"""


def floats(count):
    parts = ["uintBitsToFloat(WORDS[at + %d])" % i for i in range(count)]
    parts += ["0.0"] * (3 - count if count < 3 else 0)
    if count < 4:
        parts.append("1.0")
    return _FLOATS % ", ".join(parts)


FORMATS = {6: _UNPACK8, 25: _UNPACK16, 26: _UNPACK16_4, 36: floats(1), 37: floats(2), 57: floats(3), 38: floats(4)}


def main():
    source, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    manifest, failed = {}, {}
    for path in sorted(glob.glob(os.path.join(source, "*s_*.bin"))):
        name = os.path.basename(path)[:-4]
        data = open(path, "rb").read()
        t = Translator(data, name.startswith("ps_"), name)
        try:
            text = t.translate()
        except Unsupported as e:
            failed[name] = str(e)
            continue
        open(os.path.join(out, name + ".glsl"), "w", newline="\n").write(text)
        manifest[name] = {"fetch": t.fetch_slots, "textures": t.textures, "inputs": sorted(t.inputs) if t.is_ps else [],
                          "exports": sorted(t.exports), "colours": sorted(t.colours), "depth": t.depth, "kill": t.has_kill}
    json.dump(manifest, open(os.path.join(out, "manifest.json"), "w"), indent=0, sort_keys=True)
    print("translated", len(manifest), "failed", len(failed))
    for name, why in sorted(failed.items()):
        print("  ", name, why)


main()
