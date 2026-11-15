#!/bin/sh
# Builds uam 1.1.0 (5a5afc2) with three scheduling fixes from SwitchWakerHD (via mkdd-nx's PATCHES.md, patches
# 6-8) into /b/tools/uam. Stock uam's lit shaders came out wrong on the console (green Sim portraits): the
# scheduler read uninitialised wait data, dual-issued pairs, and let a block's first instruction pass a barrier.
set -e
apt-get update -qq >/dev/null && apt-get install -y -qq meson bison flex git python3-mako >/dev/null
rm -rf /b/uam-src && git clone -q https://github.com/devkitPro/uam /b/uam-src
cd /b/uam-src && git checkout -q 5a5afc2
python3 - <<'PY'
p = "mesa-imported/codegen/nv50_ir.cpp"
s = open(p).read()
i = s.index("Instruction::init()")
anchor = "   fixed = 0;\n"
j = s.index(anchor, i) + len(anchor)
s = s[:j] + "   sched = 0x7e0;  // patch 6: the scheduler read this before setting it\n" + s[j:]
open(p, "w").write(s)
p = "mesa-imported/codegen/nv50_ir_emit_gm107.cpp"
s = open(p).read()
a = "      if (!next) {"
assert s.count(a) == 1
s = s.replace(a, "      if (!next || insn->bb != next->bb) {  // patch 8")
b = "   if (lastDualIssued || !next || delay > 1 || !targ->canDualIssue(insn, next)) {"
assert s.count(b) == 1
s = s.replace(b, "   if (true || lastDualIssued || !next || delay > 1 || !targ->canDualIssue(insn, next)) {  // patch 7: no dual issue")
open(p, "w").write(s)
PY
git diff --stat
meson setup build --buildtype=release >/dev/null
ninja -C build >/dev/null
mkdir -p /b/tools && cp build/uam /b/tools/uam
/b/tools/uam --version
