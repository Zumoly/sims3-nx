"""Fixes applied to generated code after codegen. Safe to run repeatedly.
usage: python3 post_codegen.py <generated_dir>

Three functions dispatch through a byte-indexed jump table: sub_889DF148, sub_88D79100, sub_88DF07B8.
Codegen cannot see the table length, assumes 512 entries, and reads past the table into other data.
That pulls blocks of the next function into the switch, and those blocks branch to labels that were
never emitted. tools/switch_proof.py shows the blocks are unreachable: the bounds checks before the
dispatch stop at 17, 27 and 29, and the first case leaving each function is 83, 108 and 62.

So a goto whose label its function does not define is replaced with a trap. Nothing else changes."""
import glob, os, re, sys

gen = sys.argv[1]
func_re = re.compile(r"(?m)^(?=DEFINE_REX_FUNC\()")
label_re = re.compile(r"(?m)^\s*(loc_[0-9A-F]{8}):")
goto_re = re.compile(r"goto (loc_[0-9A-F]{8});")
EXPECTED = {"sub_889DF148", "sub_88D79100", "sub_88DF07B8"}
# Title update 1 (TU=1, Sims3.xex 0.0.1.4): the same routines, moved (found by their code in the patched image).
TU = os.environ.get("TU") == "1"
if TU:
    EXPECTED = set(os.environ.get("TU_JUMP_TABLES", "sub_889DECA8 sub_88D79700 sub_88DF10A0").split())
touched = set()

for path in glob.glob(os.path.join(gen, "*", "sims3_recomp.*.cpp")):
    data = open(path, encoding="utf-8", errors="surrogateescape", newline="").read()
    parts = func_re.split(data)
    changed = False
    for i, part in enumerate(parts):
        m = re.match(r"DEFINE_REX_FUNC\((sub_[0-9A-F]{8})\)", part)
        if not m:
            continue
        defined = set(label_re.findall(part))

        def fix(g):
            return g.group(0) if g.group(1) in defined else "__builtin_trap(); /* unreachable: see post_codegen.py */"

        new = goto_re.sub(fix, part)
        if new != part:
            parts[i] = new
            changed = True
            touched.add(m.group(1))
    if changed:
        open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write("".join(parts))

# GCC ignores attributes written before `extern "C"`, so the macro's weak alias sub_X -> __imp__sub_X
# was never emitted and every generated function came up undefined at link time. Clang accepts both.
ALIAS_OLD = re.compile(
    r'__attribute__\(\(alias\("__imp__" #name\)\)\)\s*\\\n'
    r'\s*__attribute__\(\(weak, noinline\)\) extern "C" REX_FUNC\(name\);\s*\\\n')
ALIAS_NEW = ('extern "C" __attribute__((alias("__imp__" #name)))  \\\n'
             '  __attribute__((weak, noinline)) REX_FUNC(name);    \\\n')
pch_fixed = 0
for path in glob.glob(os.path.join(gen, "*", "sims3_pch.h")):
    data = open(path, encoding="utf-8", errors="surrogateescape", newline="").read().replace("\r\n", "\n")
    new, n = ALIAS_OLD.subn(ALIAS_NEW, data)
    if n:
        open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(new)
        pch_fixed += n
print("post_codegen: GCC alias macro fixed in", pch_fixed, "headers")

# default.xex and Sims3.xex both generate xstart and the __save/__rest helpers. On PC they were separate
# DLLs. In one NRO the SDK's --allow-multiple-definition silently keeps the first copy, so Sims3.xex's
# entry ran default.xex's xstart: the loader again, and never Sims 3's CRT startup. The heap stayed null.
def defined_names(folder):
    found = set()
    for path in glob.glob(os.path.join(gen, folder, "sims3_recomp.*.cpp")):
        text = open(path, encoding="utf-8", errors="surrogateescape", newline="").read()
        found |= set(re.findall(r"(?m)^DEFINE_REX_FUNC\((\w+)\)", text))
    return found


shared = sorted(defined_names("default") & defined_names("sims3"))
renamed_files = 0
if shared:
    pattern = re.compile(r"\b(" + "|".join(map(re.escape, shared)) + r")\b")
    for path in glob.glob(os.path.join(gen, "sims3", "*")):
        if not path.endswith((".cpp", ".h")):
            continue
        text = open(path, encoding="utf-8", errors="surrogateescape", newline="").read()
        new = pattern.sub(lambda m: "s3dll_" + m.group(1), text)
        if new != text:
            open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(new)
            renamed_files += 1
print("post_codegen: renamed", len(shared), "shared names in", renamed_files, "files")

# sub_88F5C7E0 is a D3D worker that walks a job list and calls the job's callback. Creating a Sim reached
# a job with no callback, and the indirect call to 0 ended the game. The cause is not known yet, so the
# job is reported in full by sims3_NullWorkerJob (deko3d_graphics.cpp) and skipped.
NULL_JOB_LR = "0x88F5CEB8" if TU else "0x88F5C980"  # TU: sub_88F5CD18, the same bctrl at +0x19C
NULL_JOB_CALL = re.compile(r"ctx\.lr = " + NULL_JOB_LR + r";(\r?\n)\tREX_CALL_INDIRECT_FUNC\(ctx\.ctr\.u32\);")
NULL_JOB_FIELDS = "16, 20, 24, 28, 32, 36, 56, 60, 72, 76, 104, 356, 360, 364"
NULL_JOB_GUARD = (
    "ctx.lr = " + NULL_JOB_LR + ";\n"
    "\tif (ctx.ctr.u32) {\n"
    "\t\tREX_CALL_INDIRECT_FUNC(ctx.ctr.u32);\n"
    "\t} else {\n"
    "\t\tstatic const uint32_t offsets[14] = {" + NULL_JOB_FIELDS + "};\n"
    "\t\tuint32_t fields[14], stream[24];\n"
    "\t\tfor (int i = 0; i < 14; ++i) fields[i] = REX_LOAD_U32(ctx.r31.u32 + offsets[i]);\n"
    "\t\tconst uint32_t start = fields[9];\n"
    "\t\tfor (int i = 0; i < 24; ++i) stream[i] = start ? REX_LOAD_U32(start + i * 4) : 0;\n"
    "\t\tsims3_NullWorkerJob(ctx.r31.u32, ctx.r30.u32, fields, stream);\n"
    "\t}")
null_job_guards = 0
for path in glob.glob(os.path.join(gen, "sims3", "sims3_recomp.*.cpp")):
    text = open(path, encoding="utf-8", errors="surrogateescape", newline="").read()
    if "sims3_NullWorkerJob" in text:
        null_job_guards += 1
        continue
    new, n = NULL_JOB_CALL.subn(NULL_JOB_GUARD, text)
    if n:
        declaration = ('extern "C" void sims3_NullWorkerJob(uint32_t device, uint32_t worker, '
                       'const uint32_t* fields, const uint32_t* stream);\n')
        new = re.sub(r"(?m)^#include [^\n]*\n", lambda m: m.group(0) + declaration, new, count=1)
        open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(new)
        null_job_guards += n
print("post_codegen: null job guards:", null_job_guards)
if null_job_guards != 1:
    sys.exit("post_codegen: expected one null job guard, found %d" % null_job_guards)

print("post_codegen: trapped undefined labels in", sorted(touched))
# A function boundary that splits a routine leaves branches that abort at run time. Never ship those.
markers = 0
for path in glob.glob(os.path.join(gen, "*", "sims3_recomp.*.cpp")):
    markers += open(path, encoding="utf-8", errors="surrogateescape", newline="").read().count("REX_FATAL(\"Unresolved")
print("post_codegen: unresolved branch markers:", markers)
if markers:
    sys.exit("post_codegen: generated code has unresolved branches, fix function boundaries")
unexpected = touched - EXPECTED
if unexpected:
    sys.exit("post_codegen: unexpected functions, check each one: %s" % sorted(unexpected))
