#!/bin/sh
# Runs inside devkitpro/devkita64. /nx = Sims3-NX, /dump = game dump (read-only).
set -eu
SDK=/nx/third_party/katamari-nx/katamari-nx-main/sdk
B=/tmp/xexdump
mkdir -p "$B" /nx/work/images
python3 - "$SDK/src/system/xex_module.cpp" "$B/key.h" <<'EOF'
import re, sys
src = open(sys.argv[1], encoding="utf-8", errors="replace").read()
m = re.search(r"xe_xex2_retail_key\[16\]\s*=\s*\{([^}]*)\}", src)
b = re.findall(r"0x[0-9A-Fa-f]{2}", m.group(1))
assert len(b) == 16
open(sys.argv[2], "w").write("static const uint8_t retail_key[16]={%s};\n" % ",".join(b))
EOF
R=$(find "$SDK/thirdparty/crypto" -name 'rijndael-alg-fst.c' | head -1)
gcc -O2 -Wall -Wno-unused-result -I"$B" -I"$(dirname "$R")" /nx/tools/xexdump.c "$R" -o "$B/xexdump"
for n in default PrivLoader Sims3; do
  "$B/xexdump" "/dump/$n.xex" "/nx/work/images/$n.bin"
done
cd /nx/work/images && sha256sum *.bin
