#!/bin/sh
# Runs inside devkitpro/devkita64. /nx = Sims3-NX, /dump = game dump (read-only).
# Builds xexdump and xexpatch, then writes the title update's patched images to work/tu/images.
set -u
SDK=/nx/third_party/katamari-nx/katamari-nx-main/sdk
M=$SDK/thirdparty/libmspack/libmspack/mspack
B=/tmp/xexpatch
mkdir -p "$B" /nx/work/tu/images
python3 - "$SDK/src/system/xex_module.cpp" "$B/key.h" <<'PY'
import re, sys
src = open(sys.argv[1], encoding="utf-8", errors="replace").read()
m = re.search(r"xe_xex2_retail_key\[16\]\s*=\s*\{([^}]*)\}", src)
b = re.findall(r"0x[0-9A-Fa-f]{2}", m.group(1))
open(sys.argv[2], "w").write("static const uint8_t retail_key[16]={%s};\n" % ",".join(b))
PY
R=$(find "$SDK/thirdparty/crypto" -name 'rijndael-alg-fst.c' | head -1)
gcc -O2 -w -I"$B" -I"$(dirname "$R")" /nx/tools/xexdump.c "$R" -o "$B/xexdump"
gcc -O2 -w -I"$B" -I"$(dirname "$R")" -I"$M" /nx/tools/xexpatch.c "$R" "$M/lzxd.c" -o "$B/xexpatch"
for n in ${ONLY:-default PrivLoader Sims3}; do
  "$B/xexdump" "/dump/$n.xex" "$B/$n.base.bin"
  "$B/xexpatch" "/dump/$n.xex" "$B/$n.base.bin" "/nx/work/tu/xex/$n.xexp" "/nx/work/tu/images/$n.bin"
done
cd /nx/work/tu/images && sha256sum *.bin
